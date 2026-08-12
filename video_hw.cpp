#include "video/video_hw.h"

#include "core/foundation/diagnostics/log.h"

namespace nxm::video {

#if defined(NX_RHI_VULKAN)

} // namespace nxm::video

#include "core/rendering/rhi/vulkan/vk_video.h"

#include "core/foundation/containers/blob.h"
#include "core/foundation/vfs/vfs.h"

#include "mkvparser/mkvparser.h"

#include <cstring>

namespace nxm::video {
namespace {

namespace rhi = nxe::rhi;

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

/// Yields the compressed VP9 packets of a .webm in order - the bytes a hardware
/// decoder is fed, with their presentation time. No libvpx: demux only.
class WebmDemux {
public:
  ~WebmDemux() { delete m_segment; }

  [[nodiscard]] bool open(const nx::string_view path) {
    auto bytes = nx::vfs::read(nx::vfs::path_view(path));
    if (!bytes)
      return false;
    m_bytes = std::move(*bytes);
    m_reader = MemoryReader(m_bytes.data(), nx::cast<long long>(m_bytes.size()));

    long long pos = 0;
    if (mkvparser::EBMLHeader{}.Parse(&m_reader, pos) < 0)
      return false;
    if (mkvparser::Segment::CreateInstance(&m_reader, pos, m_segment) < 0 ||
        m_segment == nullptr || m_segment->Load() < 0)
      return false;

    const mkvparser::Tracks *const tracks = m_segment->GetTracks();
    const mkvparser::VideoTrack *video = nullptr;
    for (unsigned long i = 0; tracks != nullptr && i < tracks->GetTracksCount();
         ++i) {
      const mkvparser::Track *const t = tracks->GetTrackByIndex(i);
      if (t != nullptr && t->GetType() == mkvparser::Track::kVideo &&
          t->GetCodecId() != nullptr &&
          std::strcmp(t->GetCodecId(), "V_VP9") == 0) {
        video = static_cast<const mkvparser::VideoTrack *>(t);
        break;
      }
    }
    if (video == nullptr)
      return false;
    m_track = video->GetNumber();
    m_width = nx::cast<u32>(video->GetWidth());
    m_height = nx::cast<u32>(video->GetHeight());
    m_fps = video->GetFrameRate() > 0.0 ? video->GetFrameRate() : 30.0;
    restart();
    return true;
  }

  [[nodiscard]] u32 width() const noexcept { return m_width; }
  [[nodiscard]] u32 height() const noexcept { return m_height; }
  [[nodiscard]] f64 frame_rate() const noexcept { return m_fps; }

  void restart() {
    m_cluster = m_segment != nullptr ? m_segment->GetFirst() : nullptr;
    m_entry = nullptr;
  }

  [[nodiscard]] bool next(nx::vector<u8> &out, f64 &pts) {
    while (m_cluster != nullptr && !m_cluster->EOS()) {
      long status = 0;
      if (m_entry == nullptr)
        status = m_cluster->GetFirst(m_entry);
      else {
        const mkvparser::BlockEntry *n = nullptr;
        status = m_cluster->GetNext(m_entry, n);
        m_entry = n;
      }
      if (status < 0 || m_entry == nullptr || m_entry->EOS()) {
        m_cluster = m_segment->GetNext(m_cluster);
        m_entry = nullptr;
        continue;
      }
      const mkvparser::Block *const block = m_entry->GetBlock();
      if (block == nullptr || block->GetTrackNumber() != m_track ||
          block->GetFrameCount() <= 0)
        continue;
      const mkvparser::Block::Frame &f = block->GetFrame(0);
      if (f.len <= 0)
        continue;
      out.resize(nx::cast<usize>(f.len));
      if (f.Read(&m_reader, out.data()) < 0)
        continue;
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
  long long m_track = 0;
  u32 m_width = 0;
  u32 m_height = 0;
  f64 m_fps = 30.0;
};

} // namespace

struct HwVideoSource::Impl {
  rhi::VideoDecoder decoder;
  WebmDemux demux;
  nx::vector<u8> next_bytes;
  f64 next_pts = 0.0;
  bool have_next = false;
  f64 current_pts = -1.0;
  HwFrame frame;
  bool valid = false;
  bool finished = false;

  void pull_next(const bool looping) {
    if (demux.next(next_bytes, next_pts)) {
      have_next = true;
      return;
    }
    if (looping) {
      demux.restart();
      if (demux.next(next_bytes, next_pts)) {
        have_next = true;
        return;
      }
    }
    have_next = false;
  }
};

bool hw_decode_available(rhi::Device &device) {
  return device.video_decode_available();
}

HwVideoSource::HwVideoSource() : m(std::make_unique<Impl>()) {}
HwVideoSource::~HwVideoSource() = default;

bool HwVideoSource::open(rhi::Device &device, const nx::string_view path) {
  if (!m->demux.open(path)) {
    nx::logw("video: hw demux failed for '{}'", path);
    return false;
  }
  if (!m->decoder.init(device, rhi::VideoCodec::VP9, m->demux.width(),
                       m->demux.height())) {
    nx::logw("video: hw decoder init failed for '{}'", path);
    return false;
  }
  m->pull_next(false);
  m->valid = m->have_next;
  return m->valid;
}

bool HwVideoSource::valid() const noexcept { return m->valid; }
u32 HwVideoSource::width() const noexcept { return m->demux.width(); }
u32 HwVideoSource::height() const noexcept { return m->demux.height(); }
f64 HwVideoSource::frame_rate() const noexcept { return m->demux.frame_rate(); }
f64 HwVideoSource::position() const noexcept { return m->current_pts; }
bool HwVideoSource::finished() const noexcept { return m->finished; }

HwFrame HwVideoSource::frame_at(const f64 target_seconds, const bool looping) {
  bool advanced = false;
  u32 shown_w = 0;
  u32 shown_h = 0;
  // Decode every frame up to the target - VP9 is a reference chain, none can be
  // skipped - and remember the last one, which is the one to show.
  while (m->have_next && m->next_pts <= target_seconds) {
    bool shown = false;
    u32 w = 0;
    u32 h = 0;
    if (m->decoder.decode_frame(m->next_bytes.data(),
                                nx::cast<u32>(m->next_bytes.size()), shown, w,
                                h)) {
      if (shown) {
        advanced = true;
        shown_w = w;
        shown_h = h;
        m->current_pts = m->next_pts;
      }
    }
    m->pull_next(looping);
    if (!m->have_next && !looping)
      m->finished = true;
  }

  if (advanced && m->decoder.show_last()) {
    m->frame.luma = m->decoder.luma_texture();
    m->frame.chroma = m->decoder.chroma_texture();
    const rhi::Extent2D coded = m->decoder.coded_extent();
    m->frame.uv_scale = {
        coded.width != 0 ? nx::cast<f32>(shown_w) / coded.width : 1.f,
        coded.height != 0 ? nx::cast<f32>(shown_h) / coded.height : 1.f};
  }
  return m->frame;
}

#else // no hardware video decode on this backend

bool hw_decode_available(nxe::rhi::Device &) { return false; }

struct HwVideoSource::Impl {};
HwVideoSource::HwVideoSource() = default;
HwVideoSource::~HwVideoSource() = default;
bool HwVideoSource::open(nxe::rhi::Device &, nx::string_view) { return false; }
bool HwVideoSource::valid() const noexcept { return false; }
u32 HwVideoSource::width() const noexcept { return 0; }
u32 HwVideoSource::height() const noexcept { return 0; }
f64 HwVideoSource::frame_rate() const noexcept { return 0.0; }
f64 HwVideoSource::position() const noexcept { return 0.0; }
bool HwVideoSource::finished() const noexcept { return true; }
HwFrame HwVideoSource::frame_at(f64, bool) { return {}; }

#endif

} // namespace nxm::video
