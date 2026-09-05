#include "video/video_mediacodec.h"

#include "core/foundation/diagnostics/log.h"

#include <cstring>

namespace nxm::video::mediacodec_detail {

bool copy_plane_checked(u8 *const dst, const usize dst_size,
                        const u32 dst_pitch, const u8 *const src,
                        const usize src_size, const i32 src_row_stride,
                        const i32 src_pixel_stride, const u32 width,
                        const u32 height) noexcept {
  if (dst == nullptr || src == nullptr || width == 0 || height == 0 ||
      dst_pitch < width || src_row_stride <= 0 || src_pixel_stride <= 0)
    return false;

  const u64 dst_required = nx::cast<u64>(height - 1) * dst_pitch + width;
  const u64 src_required =
      nx::cast<u64>(height - 1) * nx::cast<u32>(src_row_stride) +
      nx::cast<u64>(width - 1) * nx::cast<u32>(src_pixel_stride) + 1;
  if (dst_required > dst_size || src_required > src_size)
    return false;

  for (u32 row = 0; row < height; ++row) {
    const u8 *const s =
        src + nx::cast<usize>(row) * nx::cast<u32>(src_row_stride);
    u8 *const d = dst + nx::cast<usize>(row) * dst_pitch;
    if (src_pixel_stride == 1) {
      std::memcpy(d, s, width);
    } else {
      for (u32 col = 0; col < width; ++col)
        d[col] = s[nx::cast<usize>(col) * nx::cast<u32>(src_pixel_stride)];
    }
  }
  return true;
}

} // namespace nxm::video::mediacodec_detail

#if defined(__ANDROID__)

#include "video/video_demux.h"

#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <limits>
#include <memory>
#include <unistd.h>

namespace nxm::video {
namespace {

constexpr const char *MIME_VP9 = "video/x-vnd.on2.vp9";
constexpr const char *MIME_AV1 = "video/av01";

constexpr const char *KEY_MIME = "mime";
constexpr const char *KEY_WIDTH = "width";
constexpr const char *KEY_HEIGHT = "height";
constexpr const char *KEY_CSD_0 = "csd-0";
constexpr const char *KEY_MAX_INPUT_SIZE = "max-input-size";

constexpr i64 DEQUEUE_TIMEOUT_US = 10'000;
constexpr u32 MAX_IDLE_DEQUEUES = 100;
constexpr u32 MAX_SMALL_INPUT_RETRIES = 4;

class MediaCodecSource final : public FrameSource {
public:
  ~MediaCodecSource() override {
    if (m_codec != nullptr) {
      if (m_started)
        AMediaCodec_stop(m_codec);
      AMediaCodec_delete(m_codec);
    }
    if (m_reader != nullptr)
      AImageReader_delete(
          m_reader); // owns m_window; that must not be freed too
  }

  [[nodiscard]] bool open(const nx::string_view path) {
    if (m_demux.open(path, "V_VP9")) {
      m_mime = MIME_VP9;
    } else if (m_demux.open(path, "V_AV01", "V_AV1")) {
      m_mime = MIME_AV1;
    } else {
      return false;
    }
    m_width = m_demux.width();
    m_height = m_demux.height();
    const usize max_input = m_demux.max_frame_size();
    if (max_input == 0 || max_input > MAX_VIDEO_PACKET_BYTES ||
        max_input > nx::cast<usize>(std::numeric_limits<i32>::max())) {
      nx::logw("video: unusable maximum compressed frame size ({})", max_input);
      return false;
    }

    if (__builtin_available(android 26, *)) {
      if (AImageReader_newWithUsage(
              nx::cast<i32>(m_width), nx::cast<i32>(m_height),
              AIMAGE_FORMAT_YUV_420_888, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
              4, &m_reader) != AMEDIA_OK) {
        nx::logw("video: AImageReader creation failed");
        return false;
      }
      if (AImageReader_getWindow(m_reader, &m_window) != AMEDIA_OK ||
          m_window == nullptr) {
        nx::logw("video: AImageReader has no window");
        return false;
      }
    } else {
      return false;
    }

    m_codec = AMediaCodec_createDecoderByType(m_mime);
    if (m_codec == nullptr) {
      nx::logw("video: no MediaCodec decoder for {}", m_mime);
      return false;
    }
    AMediaFormat *const fmt = AMediaFormat_new();
    if (fmt == nullptr) {
      nx::logw("video: MediaCodec format allocation failed");
      return false;
    }
    AMediaFormat_setString(fmt, KEY_MIME, m_mime);
    AMediaFormat_setInt32(fmt, KEY_WIDTH, nx::cast<i32>(m_width));
    AMediaFormat_setInt32(fmt, KEY_HEIGHT, nx::cast<i32>(m_height));
    AMediaFormat_setInt32(fmt, KEY_MAX_INPUT_SIZE, nx::cast<i32>(max_input));
    const std::span<const u8> codec_private = m_demux.codec_private();
    if (!codec_private.empty())
      AMediaFormat_setBuffer(fmt, KEY_CSD_0, codec_private.data(),
                             codec_private.size());
    const media_status_t st =
        AMediaCodec_configure(m_codec, fmt, m_window, nullptr, 0);
    AMediaFormat_delete(fmt);
    if (st != AMEDIA_OK) {
      nx::logw("video: MediaCodec configure failed ({})", nx::cast<i32>(st));
      return false;
    }
    if (AMediaCodec_start(m_codec) != AMEDIA_OK) {
      nx::logw("video: MediaCodec start failed");
      return false;
    }
    m_started = true;

    if (!decode_one(m_pending)) {
      nx::logw("video: MediaCodec produced no readable frame for '{}'", path);
      return false;
    }
    m_has_pending = true;
    nx::logd("video: '{}' MediaCodec {} {}x{}", path, m_mime, m_width,
             m_height);
    return true;
  }

  [[nodiscard]] u32 width() const noexcept override { return m_width; }
  [[nodiscard]] u32 height() const noexcept override { return m_height; }
  [[nodiscard]] f64 frame_rate() const noexcept override {
    return m_demux.frame_rate();
  }
  [[nodiscard]] f64 duration() const noexcept override {
    return m_demux.duration();
  }

  [[nodiscard]] bool next(VideoFrame &out) override {
    try {
      if (m_has_pending) {
        out = std::move(m_pending);
        m_has_pending = false;
        return true;
      }
      return decode_one(out);
    } catch (...) {
      m_failed = true;
      return false;
    }
  }

  void restart() override {
    m_demux.restart();
    if (!flush())
      m_failed = true;
  }

  [[nodiscard]] bool seek(const f64 target_seconds) override {
    if (!m_demux.seek(target_seconds))
      return false;
    return flush();
  }

private:
  [[nodiscard]] bool flush() {
    if (m_codec == nullptr || !m_started ||
        AMediaCodec_flush(m_codec) != AMEDIA_OK) {
      nx::logw("video: MediaCodec flush failed");
      return false;
    }
    m_input_eos = false;
    m_output_eos = false;
    m_failed = false;
    m_has_pending = false;
    m_pending_input.clear();
    m_small_input_retries = 0;
    for (;;) {
      AImage *img = nullptr;
      if (AImageReader_acquireNextImage(m_reader, &img) != AMEDIA_OK ||
          img == nullptr)
        break;
      AImage_delete(img);
    }
    return true;
  }

  [[nodiscard]] bool decode_one(VideoFrame &out) {
    if (m_output_eos || m_failed)
      return false;

    u32 idle_dequeues = 0;
    for (;;) {
      if (!feed_input()) {
        m_failed = true;
        return false;
      }

      AMediaCodecBufferInfo info{};
      const ssize_t idx =
          AMediaCodec_dequeueOutputBuffer(m_codec, &info, DEQUEUE_TIMEOUT_US);
      if (idx >= 0) {
        idle_dequeues = 0;
        const bool eos =
            (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        const bool config =
            (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) != 0;
        const bool render = info.size > 0 && !config;
        if (AMediaCodec_releaseOutputBuffer(m_codec, nx::cast<size_t>(idx),
                                            render) != AMEDIA_OK) {
          nx::logw("video: MediaCodec output release failed");
          m_failed = true;
          return false;
        }
        m_output_eos = eos;
        if (render) {
          if (!acquire(out, info.presentationTimeUs)) {
            nx::logw("video: MediaCodec produced an unreadable output image");
            m_failed = true;
            return false;
          }
          return true;
        }
        if (eos)
          return false;
        continue;
      }
      if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
        if (++idle_dequeues >= MAX_IDLE_DEQUEUES) {
          nx::logw("video: MediaCodec timed out draining output");
          m_failed = true;
          return false;
        }
        continue;
      }
      if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED ||
          idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
        if (++idle_dequeues >= MAX_IDLE_DEQUEUES) {
          nx::logw("video: MediaCodec repeatedly changed output metadata");
          m_failed = true;
          return false;
        }
        continue;
      }
      nx::logw("video: MediaCodec output dequeue failed ({})", idx);
      m_failed = true;
      return false;
    }
  }

  [[nodiscard]] bool feed_input() {
    if (m_input_eos)
      return true;

    if (m_pending_input.empty()) {
      const u8 *data = nullptr;
      long len = 0;
      f64 pts = 0.0;
      if (m_demux.next(data, len, pts)) {
        if (data == nullptr || len <= 0) {
          nx::logw("video: demux returned an invalid compressed frame");
          return false;
        }
        m_pending_input.assign(data, data + len);
        m_pending_pts_us = nx::cast<u64>(nx::max(pts, 0.0) * 1'000'000.0);
      }
    }

    const ssize_t idx =
        AMediaCodec_dequeueInputBuffer(m_codec, DEQUEUE_TIMEOUT_US);
    if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
      return true;
    if (idx < 0) {
      nx::logw("video: MediaCodec input dequeue failed ({})", idx);
      return false;
    }

    if (m_pending_input.empty()) {
      if (AMediaCodec_queueInputBuffer(m_codec, nx::cast<size_t>(idx), 0, 0, 0,
                                       AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) !=
          AMEDIA_OK) {
        nx::logw("video: MediaCodec rejected input EOS");
        return false;
      }
      m_input_eos = true;
      return true;
    }

    size_t cap = 0;
    u8 *const in =
        AMediaCodec_getInputBuffer(m_codec, nx::cast<size_t>(idx), &cap);
    if (in == nullptr || m_pending_input.size() > cap) {
      if (AMediaCodec_queueInputBuffer(m_codec, nx::cast<size_t>(idx), 0, 0, 0,
                                       0) != AMEDIA_OK)
        return false;
      if (++m_small_input_retries >= MAX_SMALL_INPUT_RETRIES) {
        nx::logw("video: MediaCodec input buffers are too small for {} bytes",
                 m_pending_input.size());
        return false;
      }
      return true;
    }
    std::memcpy(in, m_pending_input.data(), m_pending_input.size());
    if (AMediaCodec_queueInputBuffer(m_codec, nx::cast<size_t>(idx), 0,
                                     m_pending_input.size(), m_pending_pts_us,
                                     0) != AMEDIA_OK) {
      nx::logw("video: MediaCodec rejected a compressed frame");
      return false;
    }
    m_pending_input.clear();
    m_small_input_retries = 0;
    return true;
  }

  [[nodiscard]] bool acquire(VideoFrame &out, const i64 pts_us) {
    for (int tries = 0; tries < 64; ++tries) {
      AImage *img = nullptr;
      const media_status_t s = AImageReader_acquireNextImage(m_reader, &img);
      if (s == AMEDIA_OK && img != nullptr) {
        const bool ok = convert(out, img, pts_us);
        AImage_delete(img);
        return ok;
      }
      if (s != AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE)
        return false;
      usleep(1000);
    }
    return false;
  }

  [[nodiscard]] bool convert(VideoFrame &out, AImage *const img,
                             const i64 pts_us) {
    u8 *yd = nullptr;
    u8 *ud = nullptr;
    u8 *vd = nullptr;
    int yn = 0;
    int un = 0;
    int vn = 0;
    i32 y_row = 0;
    i32 u_row = 0;
    i32 v_row = 0;
    i32 y_pix = 1;
    i32 u_pix = 1;
    i32 v_pix = 1;
    if (__builtin_available(android 26, *)) {
      i32 image_width = 0;
      i32 image_height = 0;
      i32 planes = 0;
      if (AImage_getWidth(img, &image_width) != AMEDIA_OK ||
          AImage_getHeight(img, &image_height) != AMEDIA_OK ||
          AImage_getNumberOfPlanes(img, &planes) != AMEDIA_OK || planes < 3 ||
          image_width < nx::cast<i32>(m_width) ||
          image_height < nx::cast<i32>(m_height) ||
          AImage_getPlaneData(img, 0, &yd, &yn) != AMEDIA_OK ||
          AImage_getPlaneData(img, 1, &ud, &un) != AMEDIA_OK ||
          AImage_getPlaneData(img, 2, &vd, &vn) != AMEDIA_OK ||
          AImage_getPlaneRowStride(img, 0, &y_row) != AMEDIA_OK ||
          AImage_getPlaneRowStride(img, 1, &u_row) != AMEDIA_OK ||
          AImage_getPlaneRowStride(img, 2, &v_row) != AMEDIA_OK ||
          AImage_getPlanePixelStride(img, 0, &y_pix) != AMEDIA_OK ||
          AImage_getPlanePixelStride(img, 1, &u_pix) != AMEDIA_OK ||
          AImage_getPlanePixelStride(img, 2, &v_pix) != AMEDIA_OK)
        return false;
    } else {
      return false;
    }

    try {
      const u32 w = m_width;
      const u32 h = m_height;
      const u32 cw = w / 2 + w % 2;
      const u32 ch = h / 2 + h % 2;
      VideoFrame decoded;
      decoded.width = w;
      decoded.height = h;
      decoded.y_pitch = w;
      decoded.c_pitch = cw;
      decoded.pts = nx::max(nx::cast<f64>(pts_us) / 1e6, 0.0);
      decoded.colour = m_demux.colour();
      decoded.y.resize(nx::cast<usize>(w) * h);
      decoded.cb.resize(nx::cast<usize>(cw) * ch);
      decoded.cr.resize(nx::cast<usize>(cw) * ch);

      if (yn < 0 || un < 0 || vn < 0)
        return false;
      // Pixel stride 1 is planar (tight), 2 is semi-planar (interleaved UV).
      // The checked gather handles both and rejects inconsistent vendor
      // metadata instead of walking beyond an AImage plane.
      if (!mediacodec_detail::copy_plane_checked(
              decoded.y.data(), decoded.y.size(), w, yd, nx::cast<usize>(yn),
              y_row, y_pix, w, h) ||
          !mediacodec_detail::copy_plane_checked(
              decoded.cb.data(), decoded.cb.size(), cw, ud, nx::cast<usize>(un),
              u_row, u_pix, cw, ch) ||
          !mediacodec_detail::copy_plane_checked(
              decoded.cr.data(), decoded.cr.size(), cw, vd, nx::cast<usize>(vn),
              v_row, v_pix, cw, ch))
        return false;
      out = std::move(decoded);
      return true;
    } catch (...) {
      return false;
    }
  }

  WebmVideoDemux m_demux;
  AMediaCodec *m_codec = nullptr;
  AImageReader *m_reader = nullptr;
  ANativeWindow *m_window = nullptr;
  const char *m_mime = MIME_VP9;
  u32 m_width = 0;
  u32 m_height = 0;
  nx::vector<u8> m_pending_input;
  u64 m_pending_pts_us = 0;
  u32 m_small_input_retries = 0;
  bool m_input_eos = false;
  bool m_output_eos = false;
  bool m_failed = false;
  bool m_started = false;
  bool m_has_pending = false;
  VideoFrame m_pending;
};

} // namespace

SourcePtr open_media_codec(const nx::string_view path) {
  try {
    auto source = std::make_unique<MediaCodecSource>();
    if (source->open(path))
      return source;
    return nullptr;
  } catch (...) {
    return nullptr;
  }
}

} // namespace nxm::video

#else

namespace nxm::video {

SourcePtr open_media_codec(nx::string_view) { return nullptr; }

} // namespace nxm::video

#endif
