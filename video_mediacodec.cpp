#include "video/video_mediacodec.h"

#include "core/foundation/diagnostics/log.h"

#if defined(__ANDROID__)

#include "video/video_demux.h"

#include <android/hardware_buffer.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <cstring>
#include <memory>
#include <unistd.h>

namespace nxm::video {
namespace {

constexpr const char *MIME_VP9 = "video/x-vnd.on2.vp9";
constexpr const char *MIME_AV1 = "video/av01";

constexpr const char *KEY_MIME = "mime";
constexpr const char *KEY_WIDTH = "width";
constexpr const char *KEY_HEIGHT = "height";

constexpr i64 DEQUEUE_TIMEOUT_US = 10'000;

class MediaCodecSource final : public FrameSource {
public:
  ~MediaCodecSource() override {
    if (m_codec != nullptr) {
      AMediaCodec_stop(m_codec);
      AMediaCodec_delete(m_codec);
    }
    if (m_reader != nullptr)
      AImageReader_delete(m_reader); // owns m_window; that must not be freed too
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

    // The decoder renders into this reader's surface; we read the frame back as
    // CPU-accessible YUV_420_888 planes. A few buffers deep so the decoder is
    // not starved while we hold one.
    if (__builtin_available(android 26, *)) {
      if (AImageReader_newWithUsage(
              nx::cast<i32>(m_width), nx::cast<i32>(m_height),
              AIMAGE_FORMAT_YUV_420_888, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, 4,
              &m_reader) != AMEDIA_OK) {
        nx::logw("video: AImageReader creation failed");
        return false;
      }
      if (AImageReader_getWindow(m_reader, &m_window) != AMEDIA_OK ||
          m_window == nullptr) {
        nx::logw("video: AImageReader has no window");
        return false;
      }
    } else {
      return false; // AImage plane access needs API 26; the caller uses the CPU
    }

    m_codec = AMediaCodec_createDecoderByType(m_mime);
    if (m_codec == nullptr) {
      nx::logw("video: no MediaCodec decoder for {}", m_mime);
      return false;
    }
    AMediaFormat *const fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, KEY_MIME, m_mime);
    AMediaFormat_setInt32(fmt, KEY_WIDTH, nx::cast<i32>(m_width));
    AMediaFormat_setInt32(fmt, KEY_HEIGHT, nx::cast<i32>(m_height));
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

    if (!decode_one(m_pending)) {
      nx::logw("video: MediaCodec produced no readable frame for '{}'", path);
      return false;
    }
    m_has_pending = true;
    nx::logi("video: '{}' MediaCodec {} {}x{}", path, m_mime, m_width, m_height);
    return true;
  }

  [[nodiscard]] u32 width() const noexcept override { return m_width; }
  [[nodiscard]] u32 height() const noexcept override { return m_height; }
  [[nodiscard]] f64 frame_rate() const noexcept override {
    return m_demux.frame_rate();
  }

  [[nodiscard]] bool next(VideoFrame &out) override {
    if (m_has_pending) {
      out = std::move(m_pending);
      m_has_pending = false;
      return true;
    }
    return decode_one(out);
  }

  void restart() override {
    m_demux.restart();
    flush();
  }

  [[nodiscard]] bool seek(const f64 target_seconds) override {
    if (!m_demux.seek(target_seconds))
      return false;
    flush();
    return true;
  }

private:
  void flush() {
    if (m_codec != nullptr)
      AMediaCodec_flush(m_codec);
    m_eos_sent = false;
    m_has_pending = false;
    // Drop any images the flushed decoder already handed the reader.
    for (;;) {
      AImage *img = nullptr;
      if (AImageReader_acquireNextImage(m_reader, &img) != AMEDIA_OK ||
          img == nullptr)
        break;
      AImage_delete(img);
    }
  }

  // Feeds compressed packets in and drains one decoded frame out. False at end
  // of stream.
  [[nodiscard]] bool decode_one(VideoFrame &out) {
    for (;;) {
      feed_input();

      AMediaCodecBufferInfo info{};
      const ssize_t idx =
          AMediaCodec_dequeueOutputBuffer(m_codec, &info, DEQUEUE_TIMEOUT_US);
      if (idx >= 0) {
        const bool eos =
            (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        const bool render = info.size > 0;
        // render == true sends the frame into the reader's surface; false just
        // recycles the buffer (a codec-config or empty end-of-stream buffer).
        AMediaCodec_releaseOutputBuffer(m_codec, nx::cast<size_t>(idx), render);
        if (render && acquire(out, info.presentationTimeUs))
          return true;
        if (eos)
          return false;
        continue;
      }
      if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
        if (m_eos_sent)
          return false; // no more input and none draining: the stream is done
        continue;
      }
      // INFO_OUTPUT_FORMAT_CHANGED / _BUFFERS_CHANGED: nothing to do in surface
      // mode.
    }
  }

  void feed_input() {
    if (m_eos_sent)
      return;
    const ssize_t idx =
        AMediaCodec_dequeueInputBuffer(m_codec, DEQUEUE_TIMEOUT_US);
    if (idx < 0)
      return;

    const u8 *data = nullptr;
    long len = 0;
    f64 pts = 0.0;
    if (!m_demux.next(data, len, pts)) {
      AMediaCodec_queueInputBuffer(m_codec, nx::cast<size_t>(idx), 0, 0, 0,
                                   AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
      m_eos_sent = true;
      return;
    }

    size_t cap = 0;
    u8 *const in =
        AMediaCodec_getInputBuffer(m_codec, nx::cast<size_t>(idx), &cap);
    if (in == nullptr || nx::cast<size_t>(len) > cap) {
      AMediaCodec_queueInputBuffer(m_codec, nx::cast<size_t>(idx), 0, 0, 0, 0);
      return;
    }
    std::memcpy(in, data, nx::cast<size_t>(len));
    AMediaCodec_queueInputBuffer(m_codec, nx::cast<size_t>(idx), 0,
                                 nx::cast<size_t>(len),
                                 nx::cast<u64>(pts * 1e6), 0);
  }

  // Pulls the just-rendered frame out of the reader. The render is asynchronous,
  // so a bounded wait covers the buffer arriving after releaseOutputBuffer.
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
    i32 u_pix = 1;
    i32 v_pix = 1;
    if (__builtin_available(android 26, *)) {
      if (AImage_getPlaneData(img, 0, &yd, &yn) != AMEDIA_OK ||
          AImage_getPlaneData(img, 1, &ud, &un) != AMEDIA_OK ||
          AImage_getPlaneData(img, 2, &vd, &vn) != AMEDIA_OK)
        return false;
      AImage_getPlaneRowStride(img, 0, &y_row);
      AImage_getPlaneRowStride(img, 1, &u_row);
      AImage_getPlaneRowStride(img, 2, &v_row);
      AImage_getPlanePixelStride(img, 1, &u_pix);
      AImage_getPlanePixelStride(img, 2, &v_pix);
    } else {
      return false;
    }

    const u32 w = m_width;
    const u32 h = m_height;
    const u32 cw = (w + 1) / 2;
    const u32 ch = (h + 1) / 2;
    out.width = w;
    out.height = h;
    out.y_pitch = w;
    out.c_pitch = cw;
    out.pts = nx::cast<f64>(pts_us) / 1e6;
    out.colour = m_demux.colour();
    out.y.resize(nx::cast<usize>(w) * h);
    out.cb.resize(nx::cast<usize>(cw) * ch);
    out.cr.resize(nx::cast<usize>(cw) * ch);

    copy_plane(out.y.data(), w, yd, y_row, w, h);
    // Pixel stride 1 is planar (tight), 2 is semi-planar (interleaved UV); the
    // gather handles both, so a NV12 and an I420 output come out identical.
    gather_chroma(out.cb.data(), cw, ud, u_row, u_pix, cw, ch);
    gather_chroma(out.cr.data(), cw, vd, v_row, v_pix, cw, ch);
    return true;
  }

  static void gather_chroma(u8 *const dst, const u32 dst_pitch,
                            const u8 *const src, const i32 row_stride,
                            const i32 pixel_stride, const u32 width,
                            const u32 height) {
    for (u32 row = 0; row < height; ++row) {
      const u8 *const s = src + nx::cast<usize>(row) * nx::cast<u32>(row_stride);
      u8 *const d = dst + nx::cast<usize>(row) * dst_pitch;
      for (u32 col = 0; col < width; ++col)
        d[col] = s[nx::cast<usize>(col) * nx::cast<u32>(pixel_stride)];
    }
  }

  WebmVideoDemux m_demux;
  AMediaCodec *m_codec = nullptr;
  AImageReader *m_reader = nullptr;
  ANativeWindow *m_window = nullptr;
  const char *m_mime = MIME_VP9;
  u32 m_width = 0;
  u32 m_height = 0;
  bool m_eos_sent = false;
  bool m_has_pending = false;
  VideoFrame m_pending;
};

} // namespace

SourcePtr open_media_codec(const nx::string_view path) {
  auto source = std::make_unique<MediaCodecSource>();
  if (source->open(path))
    return source;
  return nullptr;
}

} // namespace nxm::video

#else // MediaCodec is Android-only.

namespace nxm::video {

SourcePtr open_media_codec(nx::string_view) { return nullptr; }

} // namespace nxm::video

#endif
