#pragma once

/**
 * @file video_demux.h
 * @brief WebM demux shared by every decode path (namespace nxm::video).
 */

#include "video/video_source.h"

#include "core/foundation/containers/blob.h"
#include "core/foundation/strings/utf8_string_view.h"

#include "mkvparser/mkvparser.h"

#include <cstring>
#include <span>

namespace nxm::video {

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

inline void copy_plane(u8 *dst, const u32 dst_pitch, const u8 *src,
                       const int src_pitch, const u32 width,
                       const u32 height) noexcept {
  for (u32 row = 0; row < height; ++row)
    std::memcpy(dst + nx::cast<usize>(row) * dst_pitch,
                src + nx::cast<usize>(row) * nx::cast<u32>(src_pitch), width);
}

inline void fill_i420(VideoFrame &out, const u32 w, const u32 h, const u32 cw,
                      const u32 ch, const f64 pts, const u8 *y,
                      const int y_stride, const u8 *cb, const int cb_stride,
                      const u8 *cr, const int cr_stride) {
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

/// Walks the video track of a .webm, yielding each frame's compressed bytes in
/// presentation order. Demux only: no codec, so every decode path can drive it.
class WebmVideoDemux {
public:
  WebmVideoDemux() = default;
  ~WebmVideoDemux() { delete m_segment; }
  WebmVideoDemux(const WebmVideoDemux &) = delete;
  WebmVideoDemux &operator=(const WebmVideoDemux &) = delete;

  /// Opens @p path and selects the first video track whose codec id is
  /// @p codec_id (or @p codec_id_alt, for muxers that spell it differently).
  [[nodiscard]] bool open(nx::string_view path, const char *codec_id,
                          const char *codec_id_alt = nullptr);

  [[nodiscard]] u32 width() const noexcept { return m_width; }
  [[nodiscard]] u32 height() const noexcept { return m_height; }
  [[nodiscard]] f64 frame_rate() const noexcept { return m_fps; }
  /// The track's CodecPrivate (codec setup data), empty when it carries none.
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
  nx::vector<u8> m_frame;
  nx::vector<u8> m_codec_private;
  long long m_track_number = 0;
  u32 m_width = 0;
  u32 m_height = 0;
  f64 m_fps = 30.0;
};

} // namespace nxm::video
