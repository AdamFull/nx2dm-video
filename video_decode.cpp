#include "video/video_decode.h"
#include "video/video_demux.h"

#include "core/foundation/diagnostics/log.h"

#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"
#include "vpx/vpx_image.h"

#include "gav1/decoder.h"

namespace nxm::video {
namespace {

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
    if (vpx_codec_dec_init(&m_codec, vpx_codec_vp9_dx(), nullptr, 0) !=
        VPX_CODEC_OK) {
      nx::logw("video: cannot init VP9 decoder for '{}'", path);
      return false;
    }
    m_ready = true;
    nx::logi("video: '{}' VP9 {}x{}", path, m_demux.width(), m_demux.height());
    return true;
  }

  [[nodiscard]] u32 width() const noexcept override { return m_demux.width(); }
  [[nodiscard]] u32 height() const noexcept override { return m_demux.height(); }
  [[nodiscard]] f64 frame_rate() const noexcept override {
    return m_demux.frame_rate();
  }

  [[nodiscard]] bool next(VideoFrame &out) override {
    for (;;) {
      // One decode can yield more than one showable image; drain them first.
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
        continue; // skip the bad packet rather than ending the stream
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
  void reset_decoder() {
    if (m_ready) {
      vpx_codec_destroy(&m_codec);
      m_ready = vpx_codec_dec_init(&m_codec, vpx_codec_vp9_dx(), nullptr, 0) ==
                VPX_CODEC_OK;
    }
    m_iter = nullptr;
  }

  [[nodiscard]] bool fill(VideoFrame &out, const vpx_image_t *img) {
    if (img->fmt != VPX_IMG_FMT_I420) {
      nx::logw("video: unsupported pixel format {:#x}", nx::cast<u32>(img->fmt));
      return false;
    }
    fill_i420(out, img->d_w, img->d_h, (img->d_w + 1) / 2, (img->d_h + 1) / 2,
              m_last_pts, img->planes[VPX_PLANE_Y], img->stride[VPX_PLANE_Y],
              img->planes[VPX_PLANE_U], img->stride[VPX_PLANE_U],
              img->planes[VPX_PLANE_V], img->stride[VPX_PLANE_V]);
    out.colour = m_demux.colour();
    return true;
  }

  WebmVideoDemux m_demux;
  vpx_codec_ctx_t m_codec{};
  vpx_codec_iter_t m_iter = nullptr;
  bool m_ready = false;
  f64 m_last_pts = 0.0;
};

class WebmAv1Source final : public FrameSource {
public:
  WebmAv1Source() = default;
  WebmAv1Source(const WebmAv1Source &) = delete;
  WebmAv1Source &operator=(const WebmAv1Source &) = delete;

  [[nodiscard]] bool open(const nx::string_view path) {
    // "V_AV01" is the Matroska codec id; some muxers (ffmpeg among them) write
    // "V_AV1" instead, so accept either.
    if (!m_demux.open(path, "V_AV01", "V_AV1"))
      return false;
    libgav1::DecoderSettings settings;
    settings.threads = 1;
    if (m_decoder.Init(&settings) != libgav1::kStatusOk) {
      nx::logw("video: cannot init AV1 decoder for '{}'", path);
      return false;
    }
    nx::logi("video: '{}' AV1 {}x{}", path, m_demux.width(), m_demux.height());
    return true;
  }

  [[nodiscard]] u32 width() const noexcept override { return m_demux.width(); }
  [[nodiscard]] u32 height() const noexcept override { return m_demux.height(); }
  [[nodiscard]] f64 frame_rate() const noexcept override {
    return m_demux.frame_rate();
  }

  [[nodiscard]] bool next(VideoFrame &out) override {
    for (;;) {
      const u8 *data = nullptr;
      long len = 0;
      f64 pts = 0.0;
      if (!m_demux.next(data, len, pts))
        return false;

      // libgav1 keeps no copy: `data` (owned by the demux) must live until the
      // matching DequeueFrame, which the very next line does.
      if (m_decoder.EnqueueFrame(data, nx::cast<size_t>(len), 0, nullptr) !=
          libgav1::kStatusOk) {
        nx::logw("video: AV1 enqueue error");
        continue;
      }
      const libgav1::DecoderBuffer *buf = nullptr;
      if (m_decoder.DequeueFrame(&buf) != libgav1::kStatusOk) {
        nx::logw("video: AV1 decode error");
        continue;
      }
      if (buf == nullptr)
        continue; // a decode-only frame (e.g. an altref): pull the next
      m_last_pts = pts;
      return fill(out, *buf);
    }
  }

  void restart() override {
    m_decoder.SignalEOS();
    m_demux.restart();
  }

  [[nodiscard]] bool seek(const f64 target_seconds) override {
    if (!m_demux.seek(target_seconds))
      return false;
    m_decoder.SignalEOS();
    return true;
  }

private:
  [[nodiscard]] bool fill(VideoFrame &out, const libgav1::DecoderBuffer &buf) {
    if (buf.image_format != libgav1::kImageFormatYuv420 || buf.bitdepth != 8) {
      nx::logw("video: unsupported AV1 format {}/{}bit",
               nx::cast<i32>(buf.image_format), buf.bitdepth);
      return false;
    }
    fill_i420(out, nx::cast<u32>(buf.displayed_width[0]),
              nx::cast<u32>(buf.displayed_height[0]),
              nx::cast<u32>(buf.displayed_width[1]),
              nx::cast<u32>(buf.displayed_height[1]), m_last_pts, buf.plane[0],
              buf.stride[0], buf.plane[1], buf.stride[1], buf.plane[2],
              buf.stride[2]);
    out.colour = m_demux.colour();
    return true;
  }

  WebmVideoDemux m_demux;
  libgav1::Decoder m_decoder;
  f64 m_last_pts = 0.0;
};

} // namespace

SourcePtr open_webm(const nx::string_view path) {
  // Whichever codec the file's video track carries. Each source's open()
  // declines a file whose track is not its codec, so trying VP9 then AV1 picks
  // the right one without demuxing twice up front.
  auto vp9 = std::make_unique<WebmVp9Source>();
  if (vp9->open(path))
    return vp9;
  auto av1 = std::make_unique<WebmAv1Source>();
  if (av1->open(path))
    return av1;
  nx::logw("video: '{}' has no VP9 or AV1 track", path);
  return nullptr;
}

bool webm_is_vp9(const nx::string_view path) {
  WebmVideoDemux demux;
  return demux.open(path, "V_VP9");
}

} // namespace nxm::video
