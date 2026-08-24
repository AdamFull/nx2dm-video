#include "video/video_decode.h"
#include "video/video_demux.h"

#include "core/foundation/diagnostics/log.h"

#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"
#include "vpx/vpx_image.h"

#include "gav1/decoder.h"

#include <array>
#include <limits>
#include <memory>
#include <new>

namespace nxm::video {
namespace {

struct VpxFrameBuffer {
  std::unique_ptr<u8[]> data;
  usize size = 0;
  bool in_use = false;
};

struct VpxFramePool {
  std::array<VpxFrameBuffer, VP9_MAXIMUM_REF_BUFFERS + VPX_MAXIMUM_WORK_BUFFERS>
      buffers;
  usize allocated = 0;

  void mark_free() noexcept {
    for (VpxFrameBuffer &buffer : buffers)
      buffer.in_use = false;
  }
};

extern "C" int get_vpx_frame_buffer(void *const private_data,
                                    const size_t minimum,
                                    vpx_codec_frame_buffer_t *const out) {
  auto *const pool = static_cast<VpxFramePool *>(private_data);
  if (pool == nullptr || out == nullptr || minimum == 0 ||
      minimum > MAX_VIDEO_CODEC_FRAME_BUFFER_BYTES)
    return -1;

  VpxFrameBuffer *available = nullptr;
  for (VpxFrameBuffer &buffer : pool->buffers)
    if (!buffer.in_use) {
      available = &buffer;
      break;
    }
  if (available == nullptr)
    return -1;

  if (available->size < minimum) {
    if (available->size > pool->allocated)
      return -1;
    const usize retained = pool->allocated - available->size;
    if (minimum > MAX_VIDEO_DECODER_BYTES - retained)
      return -1;
    std::unique_ptr<u8[]> replacement(new (std::nothrow) u8[minimum]{});
    if (replacement == nullptr)
      return -1;
    available->data = std::move(replacement);
    available->size = minimum;
    pool->allocated = retained + minimum;
  }

  available->in_use = true;
  out->data = available->data.get();
  out->size = available->size;
  out->priv = available;
  return 0;
}

extern "C" int release_vpx_frame_buffer(void *,
                                        vpx_codec_frame_buffer_t *const frame) {
  if (frame == nullptr || frame->priv == nullptr)
    return -1;
  static_cast<VpxFrameBuffer *>(frame->priv)->in_use = false;
  frame->data = nullptr;
  frame->size = 0;
  frame->priv = nullptr;
  return 0;
}

class WebmVp9Source final : public FrameSource {
public:
  WebmVp9Source() = default;
  ~WebmVp9Source() override {
    if (m_ready)
      vpx_codec_destroy(&m_codec);
  }
  WebmVp9Source(const WebmVp9Source &) = delete;
  WebmVp9Source &operator=(const WebmVp9Source &) = delete;

  [[nodiscard]] bool open(const nx::string_view path) {
    if (!m_demux.open(path, "V_VP9"))
      return false;
    if (!init_decoder()) {
      nx::logw("video: cannot init VP9 decoder for '{}'", path);
      return false;
    }
    nx::logi("video: '{}' VP9 {}x{}", path, m_demux.width(), m_demux.height());
    return true;
  }

  [[nodiscard]] u32 width() const noexcept override { return m_demux.width(); }
  [[nodiscard]] u32 height() const noexcept override {
    return m_demux.height();
  }
  [[nodiscard]] f64 frame_rate() const noexcept override {
    return m_demux.frame_rate();
  }
  [[nodiscard]] f64 duration() const noexcept override {
    return m_demux.duration();
  }

  [[nodiscard]] bool next(VideoFrame &out) override {
    if (!m_ready)
      return false;
    for (;;) {
      if (vpx_image_t *const img = vpx_codec_get_frame(&m_codec, &m_iter))
        return fill(out, img);
      m_iter = nullptr;

      const u8 *data = nullptr;
      long len = 0;
      f64 pts = 0.0;
      if (!m_demux.next(data, len, pts))
        return false;
      m_last_pts = pts;
      if (vpx_codec_decode(&m_codec, data, nx::cast<unsigned int>(len), nullptr,
                           0) != VPX_CODEC_OK) {
        nx::logw("video: VP9 decode error: {}", vpx_codec_error(&m_codec));
        continue;
      }
    }
  }

  void restart() override {
    reset_decoder();
    m_demux.restart();
  }

  [[nodiscard]] bool seek(const f64 target_seconds) override {
    if (!m_demux.seek(target_seconds))
      return false;
    reset_decoder();
    return m_ready;
  }

private:
  [[nodiscard]] bool init_decoder() {
    vpx_codec_dec_cfg_t config{};
    config.threads = 1;
    config.w = m_demux.width();
    config.h = m_demux.height();
    if (vpx_codec_dec_init(&m_codec, vpx_codec_vp9_dx(), &config, 0) !=
        VPX_CODEC_OK)
      return false;
    m_ready = true;
    if (vpx_codec_set_frame_buffer_functions(&m_codec, &get_vpx_frame_buffer,
                                             &release_vpx_frame_buffer,
                                             &m_frame_pool) != VPX_CODEC_OK) {
      vpx_codec_destroy(&m_codec);
      m_ready = false;
      return false;
    }
    m_iter = nullptr;
    return true;
  }

  void reset_decoder() {
    if (m_ready)
      vpx_codec_destroy(&m_codec);
    m_ready = false;
    m_frame_pool.mark_free();
    (void)init_decoder();
  }

  [[nodiscard]] bool fill(VideoFrame &out, const vpx_image_t *img) {
    if (img == nullptr)
      return false;
    if (img->fmt != VPX_IMG_FMT_I420 ||
        !valid_video_dimensions(img->d_w, img->d_h) ||
        img->d_w > m_demux.width() || img->d_h > m_demux.height()) {
      nx::logw("video: unsupported pixel format {:#x}",
               nx::cast<u32>(img->fmt));
      return false;
    }
    if (!fill_i420(out, img->d_w, img->d_h, img->d_w / 2 + img->d_w % 2,
                   img->d_h / 2 + img->d_h % 2, m_last_pts,
                   img->planes[VPX_PLANE_Y], img->stride[VPX_PLANE_Y],
                   img->planes[VPX_PLANE_U], img->stride[VPX_PLANE_U],
                   img->planes[VPX_PLANE_V], img->stride[VPX_PLANE_V]))
      return false;
    out.colour = m_demux.colour();
    return true;
  }

  WebmVideoDemux m_demux;
  vpx_codec_ctx_t m_codec{};
  VpxFramePool m_frame_pool;
  vpx_codec_iter_t m_iter = nullptr;
  bool m_ready = false;
  f64 m_last_pts = 0.0;
};

struct Gav1FramePool {
  u32 width = 0;
  u32 height = 0;
  usize allocated = 0;
};

struct Gav1FrameStorage {
  std::unique_ptr<u8[]> y;
  std::unique_ptr<u8[]> u;
  std::unique_ptr<u8[]> v;
  usize bytes = 0;
};

extern "C" Libgav1StatusCode
on_gav1_frame_size(void *const private_data, const int bitdepth,
                   const Libgav1ImageFormat format, const int width,
                   const int height, const int, const int, const int, const int,
                   const int) {
  const auto *const pool = static_cast<const Gav1FramePool *>(private_data);
  if (pool == nullptr || bitdepth != 8 || format != kLibgav1ImageFormatYuv420 ||
      width <= 0 || height <= 0 || nx::cast<u64>(width) > pool->width ||
      nx::cast<u64>(height) > pool->height ||
      !valid_video_dimensions(nx::cast<u64>(width), nx::cast<u64>(height)))
    return kLibgav1StatusResourceExhausted;
  return kLibgav1StatusOk;
}

extern "C" Libgav1StatusCode
get_gav1_frame_buffer(void *const private_data, const int bitdepth,
                      const Libgav1ImageFormat format, const int width,
                      const int height, const int left_border,
                      const int right_border, const int top_border,
                      const int bottom_border, const int stride_alignment,
                      Libgav1FrameBuffer *const out) {
  auto *const pool = static_cast<Gav1FramePool *>(private_data);
  if (pool == nullptr || out == nullptr ||
      on_gav1_frame_size(private_data, bitdepth, format, width, height,
                         left_border, right_border, top_border, bottom_border,
                         stride_alignment) != kLibgav1StatusOk)
    return kLibgav1StatusResourceExhausted;

  Libgav1FrameBufferInfo info{};
  const Libgav1StatusCode computed = Libgav1ComputeFrameBufferInfo(
      bitdepth, format, width, height, left_border, right_border, top_border,
      bottom_border, stride_alignment, &info);
  if (computed != kLibgav1StatusOk)
    return computed;
  if (info.uv_buffer_size >
      (std::numeric_limits<usize>::max() - info.y_buffer_size) / 2)
    return kLibgav1StatusResourceExhausted;
  const usize bytes = info.y_buffer_size + info.uv_buffer_size * 2;
  if (pool->allocated > MAX_VIDEO_DECODER_BYTES || bytes == 0 ||
      bytes > MAX_VIDEO_CODEC_FRAME_BUFFER_BYTES ||
      bytes > MAX_VIDEO_DECODER_BYTES - pool->allocated)
    return kLibgav1StatusResourceExhausted;

  std::unique_ptr<Gav1FrameStorage> storage(new (std::nothrow)
                                                Gav1FrameStorage);
  if (storage == nullptr)
    return kLibgav1StatusOutOfMemory;
  storage->y.reset(new (std::nothrow) u8[info.y_buffer_size]);
  if (storage->y == nullptr)
    return kLibgav1StatusOutOfMemory;
  if (info.uv_buffer_size != 0) {
    storage->u.reset(new (std::nothrow) u8[info.uv_buffer_size]);
    storage->v.reset(new (std::nothrow) u8[info.uv_buffer_size]);
    if (storage->u == nullptr || storage->v == nullptr)
      return kLibgav1StatusOutOfMemory;
  }
  storage->bytes = bytes;

  const Libgav1StatusCode set =
      Libgav1SetFrameBuffer(&info, storage->y.get(), storage->u.get(),
                            storage->v.get(), storage.get(), out);
  if (set != kLibgav1StatusOk)
    return set;
  pool->allocated += bytes;
  (void)storage.release();
  return kLibgav1StatusOk;
}

extern "C" void release_gav1_frame_buffer(void *const private_data,
                                          void *const buffer_private_data) {
  auto *const pool = static_cast<Gav1FramePool *>(private_data);
  std::unique_ptr<Gav1FrameStorage> storage(
      static_cast<Gav1FrameStorage *>(buffer_private_data));
  if (pool != nullptr && storage != nullptr) {
    pool->allocated = storage->bytes <= pool->allocated
                          ? pool->allocated - storage->bytes
                          : 0;
  }
}

class WebmAv1Source final : public FrameSource {
public:
  WebmAv1Source() = default;
  WebmAv1Source(const WebmAv1Source &) = delete;
  WebmAv1Source &operator=(const WebmAv1Source &) = delete;

  [[nodiscard]] bool open(const nx::string_view path) {
    if (!m_demux.open(path, "V_AV01", "V_AV1"))
      return false;
    m_frame_pool.width = m_demux.width();
    m_frame_pool.height = m_demux.height();
    if (!init_decoder()) {
      nx::logw("video: cannot init AV1 decoder for '{}'", path);
      return false;
    }
    nx::logi("video: '{}' AV1 {}x{}", path, m_demux.width(), m_demux.height());
    return true;
  }

  [[nodiscard]] u32 width() const noexcept override { return m_demux.width(); }
  [[nodiscard]] u32 height() const noexcept override {
    return m_demux.height();
  }
  [[nodiscard]] f64 frame_rate() const noexcept override {
    return m_demux.frame_rate();
  }
  [[nodiscard]] f64 duration() const noexcept override {
    return m_demux.duration();
  }

  [[nodiscard]] bool next(VideoFrame &out) override {
    if (m_decoder == nullptr)
      return false;
    for (;;) {
      const u8 *data = nullptr;
      long len = 0;
      f64 pts = 0.0;
      if (!m_demux.next(data, len, pts))
        return false;

      // libgav1 keeps no copy: `data` (owned by the demux) must live until the
      // matching DequeueFrame, which the very next line does.
      if (m_decoder->EnqueueFrame(data, nx::cast<size_t>(len), 0, nullptr) !=
          libgav1::kStatusOk) {
        nx::logw("video: AV1 enqueue error");
        continue;
      }
      const libgav1::DecoderBuffer *buf = nullptr;
      if (m_decoder->DequeueFrame(&buf) != libgav1::kStatusOk) {
        nx::logw("video: AV1 decode error");
        continue;
      }
      if (buf == nullptr)
        continue;
      m_last_pts = pts;
      return fill(out, *buf);
    }
  }

  void restart() override {
    m_demux.restart();
    (void)init_decoder();
  }

  [[nodiscard]] bool seek(const f64 target_seconds) override {
    if (!m_demux.seek(target_seconds))
      return false;
    return init_decoder();
  }

private:
  [[nodiscard]] bool init_decoder() {
    m_decoder.reset();
    if (m_frame_pool.allocated != 0)
      return false;
    std::unique_ptr<libgav1::Decoder> decoder(new (std::nothrow)
                                                  libgav1::Decoder);
    if (decoder == nullptr)
      return false;
    libgav1::DecoderSettings settings;
    settings.threads = 1;
    settings.on_frame_buffer_size_changed = &on_gav1_frame_size;
    settings.get_frame_buffer = &get_gav1_frame_buffer;
    settings.release_frame_buffer = &release_gav1_frame_buffer;
    settings.callback_private_data = &m_frame_pool;
    if (decoder->Init(&settings) != libgav1::kStatusOk)
      return false;
    m_decoder = std::move(decoder);
    return true;
  }

  [[nodiscard]] bool fill(VideoFrame &out, const libgav1::DecoderBuffer &buf) {
    if (buf.image_format != libgav1::kImageFormatYuv420 || buf.bitdepth != 8 ||
        buf.displayed_width[0] <= 0 || buf.displayed_height[0] <= 0 ||
        buf.displayed_width[1] <= 0 || buf.displayed_height[1] <= 0 ||
        nx::cast<u64>(buf.displayed_width[0]) > m_demux.width() ||
        nx::cast<u64>(buf.displayed_height[0]) > m_demux.height()) {
      nx::logw("video: unsupported AV1 format {}/{}bit",
               nx::cast<i32>(buf.image_format), buf.bitdepth);
      return false;
    }
    if (!fill_i420(out, nx::cast<u32>(buf.displayed_width[0]),
                   nx::cast<u32>(buf.displayed_height[0]),
                   nx::cast<u32>(buf.displayed_width[1]),
                   nx::cast<u32>(buf.displayed_height[1]), m_last_pts,
                   buf.plane[0], buf.stride[0], buf.plane[1], buf.stride[1],
                   buf.plane[2], buf.stride[2]))
      return false;
    out.colour = m_demux.colour();
    return true;
  }

  WebmVideoDemux m_demux;
  Gav1FramePool m_frame_pool;
  std::unique_ptr<libgav1::Decoder> m_decoder;
  f64 m_last_pts = 0.0;
};

} // namespace

SourcePtr open_webm(const nx::string_view path) {
  try {
    auto vp9 = std::make_unique<WebmVp9Source>();
    if (vp9->open(path))
      return vp9;
    auto av1 = std::make_unique<WebmAv1Source>();
    if (av1->open(path))
      return av1;
    nx::logw("video: '{}' has no VP9 or AV1 track", path);
    return nullptr;
  } catch (...) {
    nx::logw("video: '{}' exhausted resources while opening", path);
    return nullptr;
  }
}

bool webm_is_vp9(const nx::string_view path) {
  WebmVideoDemux demux;
  return demux.open(path, "V_VP9");
}

} // namespace nxm::video
