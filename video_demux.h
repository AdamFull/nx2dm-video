#pragma once

#include "video/video_asset.h"
#include "video/video_source.h"
#include "video/video_webm.h"

#include "core/foundation/strings/utf8_string_view.h"

#include "mkvparser/mkvparser.h"

#include <span>

namespace nxm::video {

[[nodiscard]] bool read_video_file(nx::string_view path,
                                   EncodedVideo &out) noexcept;

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
  EncodedVideo m_bytes;
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
