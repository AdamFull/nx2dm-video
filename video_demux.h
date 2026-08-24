#pragma once

#include "video/video_source.h"

#include "core/foundation/containers/blob.h"
#include "core/foundation/strings/utf8_string_view.h"

#include "mkvparser/mkvparser.h"

#include <cstring>
#include <limits>
#include <span>

namespace nxm::video {

class MemoryReader final : public mkvparser::IMkvReader {
public:
  MemoryReader(const u8 *data, long long size) noexcept
      : m_data(data), m_size(nx::max(size, 0ll)) {}

  int Read(long long pos, long len, unsigned char *buf) override {
    if (pos < 0 || len < 0 || pos > m_size || len > m_size - pos)
      return -1;
    if (len == 0)
      return 0;
    if (buf == nullptr || m_data == nullptr)
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

[[nodiscard]] bool read_video_file(nx::string_view path,
                                   nx::blob<u8> &out) noexcept;

/// Walks the video track of a .webm, yielding each frame's compressed bytes in
/// presentation order. Demux only: no codec, so every decode path can drive it.
class WebmVideoDemux {
public:
  WebmVideoDemux() = default;
  ~WebmVideoDemux() { delete m_segment; }
  WebmVideoDemux(const WebmVideoDemux &) = delete;
  WebmVideoDemux &operator=(const WebmVideoDemux &) = delete;

  [[nodiscard]] bool open(nx::string_view path, const char *codec_id,
                          const char *codec_id_alt = nullptr);

  [[nodiscard]] u32 width() const noexcept { return m_width; }
  [[nodiscard]] u32 height() const noexcept { return m_height; }
  [[nodiscard]] f64 frame_rate() const noexcept { return m_fps; }
  [[nodiscard]] f64 duration() const noexcept;
  [[nodiscard]] usize max_frame_size() const noexcept;
  [[nodiscard]] ColourInfo colour() const noexcept { return m_colour; }
  [[nodiscard]] std::span<const u8> codec_private() const noexcept {
    return {m_codec_private.data(), m_codec_private.size()};
  }

  void restart();

  [[nodiscard]] bool seek(f64 target_seconds);
  [[nodiscard]] bool next(const u8 *&data, long &len, f64 &pts);

private:
  nx::blob<u8> m_bytes;
  MemoryReader m_reader{nullptr, 0};
  mkvparser::Segment *m_segment = nullptr;
  const mkvparser::Cluster *m_cluster = nullptr;
  const mkvparser::BlockEntry *m_entry = nullptr;
  bool m_at_entry = false;
  int m_frame_index = 0;
  nx::vector<u8> m_frame;
  nx::vector<u8> m_codec_private;
  long long m_track_number = 0;
  u32 m_width = 0;
  u32 m_height = 0;
  f64 m_fps = 30.0;
  ColourInfo m_colour;
};

} // namespace nxm::video
