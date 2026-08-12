#include "video/video_decode.h"

#include "core/foundation/diagnostics/log.h"
#include "core/foundation/vfs/vfs.h"

#include "mkvparser/mkvparser.h"

#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"
#include "vpx/vpx_image.h"

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

class WebmVp9Source final : public FrameSource {
public:
  WebmVp9Source() = default;
  ~WebmVp9Source() override {
    if (m_decoder_ready)
      vpx_codec_destroy(&m_codec);
    delete m_segment;
  }

  WebmVp9Source(const WebmVp9Source &) = delete;
  WebmVp9Source &operator=(const WebmVp9Source &) = delete;

  [[nodiscard]] bool open(nx::string_view path) {
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
      if (track != nullptr &&
          track->GetType() == mkvparser::Track::kVideo &&
          track->GetCodecId() != nullptr &&
          std::strcmp(track->GetCodecId(), "V_VP9") == 0) {
        video = static_cast<const mkvparser::VideoTrack *>(track);
        break;
      }
    }
    if (video == nullptr) {
      nx::logw("video: '{}' has no VP9 track", path);
      return false;
    }
    m_track_number = video->GetNumber();
    m_width = nx::cast<u32>(video->GetWidth());
    m_height = nx::cast<u32>(video->GetHeight());
    m_fps = video->GetFrameRate();
    if (m_fps <= 0.0)
      m_fps = 30.0;

    if (vpx_codec_dec_init(&m_codec, vpx_codec_vp9_dx(), nullptr, 0) !=
        VPX_CODEC_OK) {
      nx::logw("video: cannot init VP9 decoder for '{}'", path);
      return false;
    }
    m_decoder_ready = true;
    m_cluster = m_segment->GetFirst();
    nx::logi("video: '{}' VP9 {}x{}", path, m_width, m_height);
    return true;
  }

  [[nodiscard]] u32 width() const noexcept override { return m_width; }
  [[nodiscard]] u32 height() const noexcept override { return m_height; }
  [[nodiscard]] f64 frame_rate() const noexcept override { return m_fps; }

  [[nodiscard]] bool next(VideoFrame &out) override {
    for (;;) {
      // Drain frames the last packet produced before decoding another: one
      // decode can yield more than one showable image.
      if (vpx_image_t *const img = vpx_codec_get_frame(&m_codec, &m_iter)) {
        return fill(out, img);
      }
      m_iter = nullptr;

      const u8 *data = nullptr;
      long len = 0;
      f64 pts = 0.0;
      if (!pull_video_frame(data, len, pts))
        return false;

      m_last_pts = pts;
      if (vpx_codec_decode(&m_codec, data, nx::cast<unsigned int>(len), nullptr,
                           0) != VPX_CODEC_OK) {
        nx::logw("video: decode error: {}", vpx_codec_error(&m_codec));
        // Skip the bad packet rather than ending the stream.
        continue;
      }
    }
  }

  void restart() override {
    // VP9 can only resume from a keyframe, which the first cluster begins with,
    // so a clean seek to start is a decoder reset plus a cursor rewind.
    reset_decoder();
    m_entry = nullptr;
    m_at_entry = false;
    m_cluster = m_segment != nullptr ? m_segment->GetFirst() : nullptr;
  }

  [[nodiscard]] bool seek(const f64 target_seconds) override {
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

    reset_decoder();
    m_cluster = key_cluster;
    m_entry = key_entry;
    m_at_entry = true; // pull_video_frame decodes this block before advancing
    return m_decoder_ready;
  }

private:
  void reset_decoder() {
    if (m_decoder_ready) {
      vpx_codec_destroy(&m_codec);
      m_decoder_ready =
          vpx_codec_dec_init(&m_codec, vpx_codec_vp9_dx(), nullptr, 0) ==
          VPX_CODEC_OK;
    }
    m_iter = nullptr;
  }

  [[nodiscard]] bool fill(VideoFrame &out, const vpx_image_t *img) {
    if (img->fmt != VPX_IMG_FMT_I420) {
      nx::logw("video: unsupported pixel format {:#x}",
               nx::cast<u32>(img->fmt));
      return false;
    }
    const u32 w = img->d_w;
    const u32 h = img->d_h;
    const u32 cw = (w + 1) / 2;
    const u32 ch = (h + 1) / 2;
    out.width = w;
    out.height = h;
    out.y_pitch = w;
    out.c_pitch = cw;
    out.pts = m_last_pts;
    out.y.resize(nx::cast<usize>(w) * h);
    out.cb.resize(nx::cast<usize>(cw) * ch);
    out.cr.resize(nx::cast<usize>(cw) * ch);
    copy_plane(out.y.data(), w, img->planes[VPX_PLANE_Y],
               img->stride[VPX_PLANE_Y], w, h);
    copy_plane(out.cb.data(), cw, img->planes[VPX_PLANE_U],
               img->stride[VPX_PLANE_U], cw, ch);
    copy_plane(out.cr.data(), cw, img->planes[VPX_PLANE_V],
               img->stride[VPX_PLANE_V], cw, ch);
    return true;
  }

  // Advances the cluster/block cursor to the next block on the video track and
  // reads its (single) frame into m_frame. False at end of stream.
  [[nodiscard]] bool pull_video_frame(const u8 *&data, long &len, f64 &pts) {
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

  nx::blob<u8> m_bytes;
  MemoryReader m_reader{nullptr, 0};
  mkvparser::Segment *m_segment = nullptr;
  const mkvparser::Cluster *m_cluster = nullptr;
  const mkvparser::BlockEntry *m_entry = nullptr;
  bool m_at_entry = false;

  vpx_codec_ctx_t m_codec{};
  vpx_codec_iter_t m_iter = nullptr;
  bool m_decoder_ready = false;

  nx::vector<u8> m_frame;
  long long m_track_number = 0;
  u32 m_width = 0;
  u32 m_height = 0;
  f64 m_fps = 30.0;
  f64 m_last_pts = 0.0;
};

} // namespace

SourcePtr open_webm(const nx::string_view path) {
  auto source = std::make_unique<WebmVp9Source>();
  if (!source->open(path))
    return nullptr;
  return source;
}

} // namespace nxm::video
