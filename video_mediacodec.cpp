#include "video/video_mediacodec.h"

#include "core/foundation/diagnostics/log.h"

#if defined(__ANDROID__)

#include "video/video_demux.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <cstring>
#include <memory>

namespace nxm::video {
namespace {

// MediaCodec MIME types for the WebM video codecs.
constexpr const char *MIME_VP9 = "video/x-vnd.on2.vp9";
constexpr const char *MIME_AV1 = "video/av01";

constexpr const char *KEY_MIME = "mime";
constexpr const char *KEY_WIDTH = "width";
constexpr const char *KEY_HEIGHT = "height";
constexpr const char *KEY_COLOR_FORMAT = "color-format";
constexpr const char *KEY_STRIDE = "stride";
constexpr const char *KEY_SLICE_HEIGHT = "slice-height";

constexpr i32 COLOR_FormatYUV420Planar = 19;     // I420: Y, then U, then V.
constexpr i32 COLOR_FormatYUV420SemiPlanar = 21; // NV12: Y, then interleaved UV.
constexpr i32 COLOR_FormatYUV420Flexible = 0x7f420888;

constexpr i64 DEQUEUE_TIMEOUT_US = 10'000;

class MediaCodecSource final : public FrameSource {
public:
  ~MediaCodecSource() override {
    if (m_codec != nullptr) {
      AMediaCodec_stop(m_codec);
      AMediaCodec_delete(m_codec);
    }
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
    m_stride = m_width;
    m_slice_height = m_height;

    m_codec = AMediaCodec_createDecoderByType(m_mime);
    if (m_codec == nullptr) {
      nx::logw("video: no MediaCodec decoder for {}", m_mime);
      return false;
    }

    AMediaFormat *const fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, KEY_MIME, m_mime);
    AMediaFormat_setInt32(fmt, KEY_WIDTH, nx::cast<i32>(m_width));
    AMediaFormat_setInt32(fmt, KEY_HEIGHT, nx::cast<i32>(m_height));
    AMediaFormat_setInt32(fmt, KEY_COLOR_FORMAT, COLOR_FormatYUV420Flexible);
    const media_status_t st =
        AMediaCodec_configure(m_codec, fmt, nullptr, nullptr, 0);
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
  }

  void read_output_format() {
    AMediaFormat *const of = AMediaCodec_getOutputFormat(m_codec);
    if (of == nullptr)
      return;
    i32 v = 0;
    if (AMediaFormat_getInt32(of, KEY_COLOR_FORMAT, &v))
      m_color_format = v;
    if (AMediaFormat_getInt32(of, KEY_STRIDE, &v) && v > 0)
      m_stride = nx::cast<u32>(v);
    if (AMediaFormat_getInt32(of, KEY_SLICE_HEIGHT, &v) && v > 0)
      m_slice_height = nx::cast<u32>(v);
    AMediaFormat_delete(of);
  }

  // Feeds compressed packets in and drains one decoded frame out. Returns false
  // at end of stream or if the device's colour format is not one we can read.
  [[nodiscard]] bool decode_one(VideoFrame &out) {
    for (;;) {
      feed_input();

      AMediaCodecBufferInfo info{};
      const ssize_t idx =
          AMediaCodec_dequeueOutputBuffer(m_codec, &info, DEQUEUE_TIMEOUT_US);
      if (idx >= 0) {
        const bool eos =
            (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        bool got = false;
        if (info.size > 0) {
          size_t cap = 0;
          const u8 *const buf = AMediaCodec_getOutputBuffer(
              m_codec, nx::cast<size_t>(idx), &cap);
          if (buf != nullptr)
            got = convert(out, buf + info.offset, info.presentationTimeUs);
        }
        AMediaCodec_releaseOutputBuffer(m_codec, nx::cast<size_t>(idx), false);
        if (got)
          return true;
        if (m_format_bad || eos)
          return false;
        continue;
      }
      if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        read_output_format();
        continue;
      }
      if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
        if (m_eos_sent)
          return false; // no more input and none draining: the stream is done
        continue;
      }
      // INFO_OUTPUT_BUFFERS_CHANGED (deprecated) or a transient error: retry.
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
    u8 *const in = AMediaCodec_getInputBuffer(m_codec, nx::cast<size_t>(idx),
                                              &cap);
    if (in == nullptr || nx::cast<size_t>(len) > cap) {
      AMediaCodec_queueInputBuffer(m_codec, nx::cast<size_t>(idx), 0, 0, 0, 0);
      return;
    }
    std::memcpy(in, data, nx::cast<size_t>(len));
    AMediaCodec_queueInputBuffer(m_codec, nx::cast<size_t>(idx), 0,
                                 nx::cast<size_t>(len),
                                 nx::cast<u64>(pts * 1e6), 0);
  }

  [[nodiscard]] bool convert(VideoFrame &out, const u8 *const base,
                             const i64 pts_us) {
    const u32 w = m_width;
    const u32 h = m_height;
    const u32 cw = (w + 1) / 2;
    const u32 ch = (h + 1) / 2;
    const u32 ys = nx::max(m_stride, w);
    const u32 sh = nx::max(m_slice_height, h);
    const f64 pts = nx::cast<f64>(pts_us) / 1e6;
    const u8 *const y = base;
    const u8 *const chroma = base + nx::cast<usize>(ys) * sh;

    if (m_color_format == COLOR_FormatYUV420Planar) {
      const u32 cs = ys / 2; // planar chroma stride is half the luma stride
      const u32 csh = sh / 2;
      const u8 *const u = chroma;
      const u8 *const v = chroma + nx::cast<usize>(cs) * csh;
      fill_i420(out, w, h, cw, ch, pts, y, nx::cast<int>(ys), u,
                nx::cast<int>(cs), v, nx::cast<int>(cs));
      return true;
    }
    if (m_color_format == COLOR_FormatYUV420SemiPlanar) {
      out.width = w;
      out.height = h;
      out.y_pitch = w;
      out.c_pitch = cw;
      out.pts = pts;
      out.y.resize(nx::cast<usize>(w) * h);
      out.cb.resize(nx::cast<usize>(cw) * ch);
      out.cr.resize(nx::cast<usize>(cw) * ch);
      copy_plane(out.y.data(), w, y, nx::cast<int>(ys), w, h);
      for (u32 r = 0; r < ch; ++r) {
        const u8 *const row = chroma + nx::cast<usize>(r) * ys;
        u8 *const du = out.cb.data() + nx::cast<usize>(r) * cw;
        u8 *const dv = out.cr.data() + nx::cast<usize>(r) * cw;
        for (u32 c = 0; c < cw; ++c) {
          du[c] = row[2 * c];
          dv[c] = row[2 * c + 1];
        }
      }
      return true;
    }

    if (!m_format_bad)
      nx::logw("video: MediaCodec colour format {:#x} is not readable; using "
               "the CPU decoder",
               nx::cast<u32>(m_color_format));
    m_format_bad = true;
    return false;
  }

  WebmVideoDemux m_demux;
  AMediaCodec *m_codec = nullptr;
  const char *m_mime = MIME_VP9;
  u32 m_width = 0;
  u32 m_height = 0;
  u32 m_stride = 0;
  u32 m_slice_height = 0;
  i32 m_color_format = COLOR_FormatYUV420Flexible;
  bool m_eos_sent = false;
  bool m_format_bad = false;
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
