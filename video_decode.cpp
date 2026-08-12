#include "video/video_decode.h"

#include "core/foundation/diagnostics/log.h"
#include "core/foundation/vfs/vfs.h"

#include "mkvparser/mkvparser.h"

#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"
#include "vpx/vpx_image.h"

#include "gav1/decoder.h"

#include <cstring>

namespace nxm::video {
namespace {

/// mkvparser reads through this rather than fopen: on Android the .webm is
/// inside the APK, where there is no path to open. The whole file is already in
/// memory (the VFS handed it over), so a read is a bounds check and a memcpy.
class MemoryReader final : public mkvparser::IMkvReader {
public:
  MemoryReader(const u8 *data, long long size) noexcept
      : m_data(data), m_size(size) {}

  int Read(long long pos, long len, unsigned char *buf) override {
    if (pos < 0 || len < 0 || pos + len > m_size)
      return -1;
    std::memcpy(buf, m_data + pos, static_cast<size_t>(len));
    return 0;
  }
  int Length(long long *total, long long *available) override {
    if (total != nullptr)
      *total = m_size;
    if (available != nullptr)
      *available = m_size;
    return 0;
  }

private:
  const u8 *m_data;
  long long m_size;
};

void copy_plane(u8 *dst, const u32 dst_pitch, const u8 *src, const int src_pitch,
                const u32 width, const u32 height) noexcept {
  for (u32 row = 0; row < height; ++row)
    std::memcpy(dst + nx::cast<usize>(row) * dst_pitch,
                src + nx::cast<usize>(row) * nx::cast<u32>(src_pitch), width);
}

class WebmVideoDemux {
public:
  WebmVideoDemux() = default;
  ~WebmVideoDemux() { delete m_segment; }
  WebmVideoDemux(const WebmVideoDemux &) = delete;
  WebmVideoDemux &operator=(const WebmVideoDemux &) = delete;

  [[nodiscard]] bool open(const nx::string_view path, const char *const codec_id,
                          const char *const codec_id_alt = nullptr) {
    auto bytes = nx::vfs::read(nx::vfs::path_view(path));
    if (!bytes) {
      nx::logw("video: cannot read '{}'", path);
      return false;
    }
    m_bytes = std::move(*bytes);
    m_reader = MemoryReader(m_bytes.data(), nx::cast<long long>(m_bytes.size()));

    long long pos = 0;
    if (mkvparser::EBMLHeader{}.Parse(&m_reader, pos) < 0) {
      nx::logw("video: '{}' is not WebM", path);
      return false;
    }
    if (mkvparser::Segment::CreateInstance(&m_reader, pos, m_segment) < 0 ||
        m_segment == nullptr || m_segment->Load() < 0) {
      nx::logw("video: '{}' has no readable segment", path);
      return false;
    }

    const mkvparser::Tracks *const tracks = m_segment->GetTracks();
    const mkvparser::VideoTrack *video = nullptr;
    for (unsigned long i = 0; tracks != nullptr && i < tracks->GetTracksCount();
         ++i) {
      const mkvparser::Track *const track = tracks->GetTrackByIndex(i);
      const char *const cid =
          track != nullptr ? track->GetCodecId() : nullptr;
      if (track != nullptr && track->GetType() == mkvparser::Track::kVideo &&
          cid != nullptr &&
          (std::strcmp(cid, codec_id) == 0 ||
           (codec_id_alt != nullptr && std::strcmp(cid, codec_id_alt) == 0))) {
        video = static_cast<const mkvparser::VideoTrack *>(track);
        break;
      }
    }
    if (video == nullptr)
      return false;
    m_track_number = video->GetNumber();
    m_width = nx::cast<u32>(video->GetWidth());
    m_height = nx::cast<u32>(video->GetHeight());
    m_fps = video->GetFrameRate();
    if (m_fps <= 0.0)
      m_fps = 30.0;
    m_cluster = m_segment->GetFirst();
    return true;
  }

  [[nodiscard]] u32 width() const noexcept { return m_width; }
  [[nodiscard]] u32 height() const noexcept { return m_height; }
  [[nodiscard]] f64 frame_rate() const noexcept { return m_fps; }

  void restart() {
    m_entry = nullptr;
    m_at_entry = false;
    m_cluster = m_segment != nullptr ? m_segment->GetFirst() : nullptr;
  }

  // Reposition to the keyframe at or before @p target seconds; the codec reset
  // is the caller's, since it owns the decoder. False only if there is no
  // segment.
  [[nodiscard]] bool seek(const f64 target_seconds) {
    if (m_segment == nullptr)
      return false;
    const long long target_ns =
        nx::cast<long long>(nx::max(target_seconds, 0.0) * 1e9);

    const mkvparser::Cluster *key_cluster = nullptr;
    const mkvparser::BlockEntry *key_entry = nullptr;
    bool passed = false;
    for (const mkvparser::Cluster *cluster = m_segment->GetFirst();
         cluster != nullptr && !cluster->EOS() && !passed;
         cluster = m_segment->GetNext(cluster)) {
      const mkvparser::BlockEntry *entry = nullptr;
      long status = cluster->GetFirst(entry);
      while (status >= 0 && entry != nullptr && !entry->EOS()) {
        const mkvparser::Block *const block = entry->GetBlock();
        if (block != nullptr && block->GetTrackNumber() == m_track_number) {
          if (block->GetTime(cluster) > target_ns) {
            passed = true;
            break;
          }
          if (block->IsKey()) {
            key_cluster = cluster;
            key_entry = entry;
          }
        }
        const mkvparser::BlockEntry *next = nullptr;
        status = cluster->GetNext(entry, next);
        entry = next;
      }
    }

    if (key_entry == nullptr) {
      restart(); // target before the first keyframe: the start is the answer
      return true;
    }
    m_cluster = key_cluster;
    m_entry = key_entry;
    m_at_entry = true; // next() decodes this block before advancing
    return true;
  }

  // Reads the next block's (single) frame on the video track into an internal
  // buffer, valid until the following call. False at end of stream.
  [[nodiscard]] bool next(const u8 *&data, long &len, f64 &pts) {
    while (m_cluster != nullptr && !m_cluster->EOS()) {
      long status = 0;
      if (m_at_entry) {
        m_at_entry = false; // a seek left m_entry on the block to decode next
      } else if (m_entry == nullptr) {
        status = m_cluster->GetFirst(m_entry);
      } else {
        const mkvparser::BlockEntry *next = nullptr;
        status = m_cluster->GetNext(m_entry, next);
        m_entry = next;
      }
      if (status < 0 || m_entry == nullptr || m_entry->EOS()) {
        m_cluster = m_segment->GetNext(m_cluster);
        m_entry = nullptr;
        continue;
      }

      const mkvparser::Block *const block = m_entry->GetBlock();
      if (block == nullptr || block->GetTrackNumber() != m_track_number ||
          block->GetFrameCount() <= 0)
        continue;

      const mkvparser::Block::Frame &frame = block->GetFrame(0);
      if (frame.len <= 0)
        continue;
      m_frame.resize(nx::cast<usize>(frame.len));
      if (frame.Read(&m_reader, m_frame.data()) < 0)
        continue;
      data = m_frame.data();
      len = frame.len;
      pts = nx::cast<f64>(block->GetTime(m_cluster)) / 1e9;
      return true;
    }
    return false;
  }

private:
  nx::blob<u8> m_bytes;
  MemoryReader m_reader{nullptr, 0};
  mkvparser::Segment *m_segment = nullptr;
  const mkvparser::Cluster *m_cluster = nullptr;
  const mkvparser::BlockEntry *m_entry = nullptr;
  bool m_at_entry = false;
  nx::vector<u8> m_frame;
  long long m_track_number = 0;
  u32 m_width = 0;
  u32 m_height = 0;
  f64 m_fps = 30.0;
};

void fill_i420(VideoFrame &out, const u32 w, const u32 h, const u32 cw,
               const u32 ch, const f64 pts, const u8 *y, const int y_stride,
               const u8 *cb, const int cb_stride, const u8 *cr,
               const int cr_stride) {
  out.width = w;
  out.height = h;
  out.y_pitch = w;
  out.c_pitch = cw;
  out.pts = pts;
  out.y.resize(nx::cast<usize>(w) * h);
  out.cb.resize(nx::cast<usize>(cw) * ch);
  out.cr.resize(nx::cast<usize>(cw) * ch);
  copy_plane(out.y.data(), w, y, y_stride, w, h);
  copy_plane(out.cb.data(), cw, cb, cb_stride, cw, ch);
  copy_plane(out.cr.data(), cw, cr, cr_stride, cw, ch);
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
