#include "video/video_demux.h"

#include "core/foundation/diagnostics/log.h"
#include "core/foundation/vfs/vfs.h"

namespace nxm::video {
namespace {

[[nodiscard]] ColourInfo classify_colour(const mkvparser::VideoTrack &track,
                                         const u32 height) {
  ColourInfo out;
  const mkvparser::Colour *const c = track.GetColour();
  const long long mc =
      c != nullptr ? c->matrix_coefficients : mkvparser::Colour::kValueNotPresent;
  const long long range =
      c != nullptr ? c->range : mkvparser::Colour::kValueNotPresent;

  switch (mc) {
  case 1: // BT.709
    out.matrix = ColourMatrix::BT709;
    break;
  case 4: // FCC
  case 5: // BT.470BG (BT.601 625)
  case 6: // SMPTE 170M (BT.601 525)
    out.matrix = ColourMatrix::BT601;
    break;
  case 9:  // BT.2020 non-constant luminance
  case 10: // BT.2020 constant luminance
    out.matrix = ColourMatrix::BT2020;
    break;
  default: // unspecified or absent
    out.matrix = height >= 720 ? ColourMatrix::BT709 : ColourMatrix::BT601;
    break;
  }
  out.full_range = range == 2; // 2 = full; 1 = broadcast; 0/absent = broadcast
  return out;
}

} // namespace

bool WebmVideoDemux::open(const nx::string_view path, const char *const codec_id,
                          const char *const codec_id_alt) {
  auto bytes = nx::vfs::read(nx::vfs::path_view(path));
  if (!bytes) {
    nx::logw("video: cannot read '{}'", path);
    return false;
  }
  m_bytes = std::move(*bytes);
  m_reader = MemoryReader(m_bytes.data(), nx::cast<long long>(m_bytes.size()));

  // Re-openable: a caller may try one codec id, fail to find its track, and try
  // another on the same demux (the MediaCodec source does, VP9 then AV1).
  delete m_segment;
  m_segment = nullptr;
  m_codec_private.clear();

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
    const char *const cid = track != nullptr ? track->GetCodecId() : nullptr;
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
  m_colour = classify_colour(*video, m_height);
  size_t priv_size = 0;
  if (const u8 *const priv = video->GetCodecPrivate(priv_size);
      priv != nullptr && priv_size != 0)
    m_codec_private.assign(priv, priv + priv_size);
  m_cluster = m_segment->GetFirst();
  return true;
}

f64 WebmVideoDemux::duration() const noexcept {
  return m_segment != nullptr
             ? nx::max(nx::cast<f64>(m_segment->GetDuration()) / 1e9, 0.0)
             : 0.0;
}

usize WebmVideoDemux::max_frame_size() const noexcept {
  usize largest = 0;
  if (m_segment == nullptr)
    return largest;
  for (const mkvparser::Cluster *cluster = m_segment->GetFirst();
       cluster != nullptr && !cluster->EOS();
       cluster = m_segment->GetNext(cluster)) {
    const mkvparser::BlockEntry *entry = nullptr;
    long status = cluster->GetFirst(entry);
    while (status >= 0 && entry != nullptr && !entry->EOS()) {
      const mkvparser::Block *const block = entry->GetBlock();
      if (block != nullptr && block->GetTrackNumber() == m_track_number)
        for (int i = 0; i < block->GetFrameCount(); ++i) {
          const long len = block->GetFrame(i).len;
          if (len > 0)
            largest = nx::max(largest, nx::cast<usize>(len));
        }
      const mkvparser::BlockEntry *next = nullptr;
      status = cluster->GetNext(entry, next);
      entry = next;
    }
  }
  return largest;
}

void WebmVideoDemux::restart() {
  m_entry = nullptr;
  m_at_entry = false;
  m_cluster = m_segment != nullptr ? m_segment->GetFirst() : nullptr;
}

bool WebmVideoDemux::seek(const f64 target_seconds) {
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

bool WebmVideoDemux::next(const u8 *&data, long &len, f64 &pts) {
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

} // namespace nxm::video
