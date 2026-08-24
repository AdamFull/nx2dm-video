#include "video/video_demux.h"

#include "core/foundation/diagnostics/log.h"
#include "core/foundation/vfs/vfs.h"

#include <cmath>
#include <limits>

namespace nxm::video {
namespace {

[[nodiscard]] ColourInfo classify_colour(const mkvparser::VideoTrack &track,
                                         const u32 height) {
  ColourInfo out;
  const mkvparser::Colour *const c = track.GetColour();
  const long long mc = c != nullptr ? c->matrix_coefficients
                                    : mkvparser::Colour::kValueNotPresent;
  const long long range =
      c != nullptr ? c->range : mkvparser::Colour::kValueNotPresent;

  switch (mc) {
  case 1:
    out.matrix = ColourMatrix::BT709;
    break;
  case 4:
  case 5:
  case 6:
    out.matrix = ColourMatrix::BT601;
    break;
  case 9:
  case 10:
    out.matrix = ColourMatrix::BT2020;
    break;
  default:
    out.matrix = height >= 720 ? ColourMatrix::BT709 : ColourMatrix::BT601;
    break;
  }
  out.full_range = range == 2;
  return out;
}

} // namespace

bool read_video_file(const nx::string_view path, nx::blob<u8> &out) noexcept {
  try {
    const nx::vfs::FileInfo info = nx::vfs::stat(path);
    if (info.exists && !info.is_directory &&
        info.size > MAX_ENCODED_VIDEO_BYTES) {
      nx::logw("video: '{}' exceeds the encoded-size limit", path);
      return false;
    }
    auto bytes = nx::vfs::read(nx::vfs::path_view(path));
    if (!bytes) {
      nx::logw("video: cannot read '{}'", path);
      return false;
    }
    if (bytes->size() > MAX_ENCODED_VIDEO_BYTES) {
      nx::logw("video: '{}' exceeds the encoded-size limit", path);
      return false;
    }
    out = std::move(*bytes);
    return true;
  } catch (...) {
    return false;
  }
}

bool WebmVideoDemux::open(const nx::string_view path,
                          const char *const codec_id,
                          const char *const codec_id_alt) {
  if (codec_id == nullptr)
    return false;

  delete m_segment;
  m_segment = nullptr;
  m_bytes = {};
  m_reader = MemoryReader(nullptr, 0);
  m_codec_private.clear();
  m_cluster = nullptr;
  m_entry = nullptr;
  m_at_entry = false;
  m_frame_index = 0;
  m_track_number = 0;
  m_width = 0;
  m_height = 0;
  m_fps = 30.0;
  m_colour = {};

  try {
    if (!read_video_file(path, m_bytes))
      return false;
    m_reader =
        MemoryReader(m_bytes.data(), nx::cast<long long>(m_bytes.size()));

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

    const long long width = video->GetWidth();
    const long long height = video->GetHeight();
    const long long track_number = video->GetNumber();
    if (width <= 0 || height <= 0 || track_number <= 0 ||
        !valid_video_dimensions(nx::cast<u64>(width), nx::cast<u64>(height))) {
      nx::logw("video: '{}' has unusable dimensions or track metadata", path);
      return false;
    }
    m_width = nx::cast<u32>(width);
    m_height = nx::cast<u32>(height);
    m_track_number = track_number;

    const f64 fps = video->GetFrameRate();
    if (!std::isfinite(fps) || fps <= 0.0) {
      m_fps = 30.0;
    } else if (fps > MAX_VIDEO_FRAME_RATE) {
      nx::logw("video: '{}' declares an unusable frame rate ({})", path, fps);
      return false;
    } else {
      m_fps = fps;
    }

    m_colour = classify_colour(*video, m_height);
    size_t priv_size = 0;
    if (const u8 *const priv = video->GetCodecPrivate(priv_size);
        priv != nullptr && priv_size != 0) {
      if (priv_size > MAX_VIDEO_CODEC_PRIVATE_BYTES)
        return false;
      m_codec_private.assign(priv, priv + priv_size);
    }
    m_cluster = m_segment->GetFirst();
    return m_cluster != nullptr;
  } catch (...) {
    nx::logw("video: '{}' exhausted resources while opening", path);
    return false;
  }
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
    if (status < 0)
      break;
  }
  return largest;
}

void WebmVideoDemux::restart() {
  m_entry = nullptr;
  m_at_entry = false;
  m_frame_index = 0;
  m_cluster = m_segment != nullptr ? m_segment->GetFirst() : nullptr;
}

bool WebmVideoDemux::seek(const f64 target_seconds) {
  if (m_segment == nullptr || !std::isfinite(target_seconds) ||
      target_seconds < 0.0 ||
      target_seconds >
          nx::cast<f64>(std::numeric_limits<long long>::max()) / 1e9)
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
    if (status < 0)
      return false;
  }

  if (key_entry == nullptr) {
    restart();
    return true;
  }
  m_cluster = key_cluster;
  m_entry = key_entry;
  m_at_entry = true;
  m_frame_index = 0;
  return true;
}

bool WebmVideoDemux::next(const u8 *&data, long &len, f64 &pts) {
  data = nullptr;
  len = 0;
  pts = 0.0;
  while (m_cluster != nullptr && !m_cluster->EOS()) {
    if (m_entry != nullptr) {
      const mkvparser::Block *const block = m_entry->GetBlock();
      if (block != nullptr && block->GetTrackNumber() == m_track_number &&
          m_frame_index >= 0 && m_frame_index < block->GetFrameCount()) {
        m_at_entry = false;
        const mkvparser::Block::Frame &frame = block->GetFrame(m_frame_index++);
        if (frame.len <= 0)
          continue;
        if (nx::cast<usize>(frame.len) > MAX_VIDEO_PACKET_BYTES)
          return false;
        try {
          m_frame.resize(nx::cast<usize>(frame.len));
        } catch (...) {
          return false;
        }
        if (frame.Read(&m_reader, m_frame.data()) < 0)
          return false;
        const f64 time = nx::cast<f64>(block->GetTime(m_cluster)) / 1e9;
        if (!std::isfinite(time))
          return false;
        data = m_frame.data();
        len = frame.len;
        pts = nx::max(time, 0.0);
        return true;
      }
    }

    long status = 0;
    if (m_at_entry) {
      m_at_entry = false;
    } else if (m_entry == nullptr) {
      status = m_cluster->GetFirst(m_entry);
    } else {
      const mkvparser::BlockEntry *next = nullptr;
      status = m_cluster->GetNext(m_entry, next);
      m_entry = next;
    }
    if (status < 0)
      return false;
    if (m_entry == nullptr || m_entry->EOS()) {
      m_cluster = m_segment->GetNext(m_cluster);
      m_entry = nullptr;
      m_frame_index = 0;
      continue;
    }

    const mkvparser::Block *const block = m_entry->GetBlock();
    if (block == nullptr || block->GetTrackNumber() != m_track_number ||
        block->GetFrameCount() <= 0)
      continue;
    m_frame_index = 0;
  }
  return false;
}

} // namespace nxm::video
