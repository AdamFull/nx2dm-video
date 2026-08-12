#pragma once

/**
 * @file video_source.h
 * @brief Decoded frames, and where they come from (namespace nxm::video).
 */

#include "core/foundation/containers/blob.h"
#include "core/foundation/core/foundation.h"

#include <memory>

namespace nxm::video {

/// One decoded frame: planar Y'CbCr 4:2:0, tightly packed. Chroma is
/// (width+1)/2 by (height+1)/2. `pts` is the presentation time in seconds.
struct VideoFrame {
  u32 width = 0;
  u32 height = 0;
  u32 y_pitch = 0;
  u32 c_pitch = 0;
  f64 pts = 0.0;
  nx::vector<u8> y;
  nx::vector<u8> cb;
  nx::vector<u8> cr;

  [[nodiscard]] u32 chroma_width() const noexcept { return (width + 1) / 2; }
  [[nodiscard]] u32 chroma_height() const noexcept { return (height + 1) / 2; }
};

/// A source of decoded frames. The always-available implementation is CPU
/// software decode (libwebm + libvpx); the synthetic one drives the whole
/// pipeline without a codec, which is what proves the engine side end to end.
class FrameSource {
public:
  virtual ~FrameSource() = default;

  [[nodiscard]] virtual u32 width() const noexcept = 0;
  [[nodiscard]] virtual u32 height() const noexcept = 0;
  [[nodiscard]] virtual f64 frame_rate() const noexcept = 0;

  /// Fills @p out with the next frame. False at end of stream, before any loop.
  [[nodiscard]] virtual bool next(VideoFrame &out) = 0;

  /// Seek back to the first frame.
  virtual void restart() = 0;
};

/// std::unique_ptr rather than nx::unique_ptr, deliberately: it owns an
/// interface, which AllocDeleter cannot. Same reason the audio DecoderPtr does.
using SourcePtr = std::unique_ptr<FrameSource>;

/// An animated test pattern: a diagonal luma sweep over a fixed chroma field.
/// Endless; `next` never returns false.
class SyntheticSource final : public FrameSource {
public:
  SyntheticSource(u32 width, u32 height, f64 fps) noexcept;

  [[nodiscard]] u32 width() const noexcept override { return m_width; }
  [[nodiscard]] u32 height() const noexcept override { return m_height; }
  [[nodiscard]] f64 frame_rate() const noexcept override { return m_fps; }

  [[nodiscard]] bool next(VideoFrame &out) override;
  void restart() override { m_frame = 0; }

private:
  u32 m_width;
  u32 m_height;
  f64 m_fps;
  u64 m_frame = 0;
};

class PacedPlayback {
public:
  PacedPlayback() = default;

  PacedPlayback(const PacedPlayback &) = delete;
  PacedPlayback &operator=(const PacedPlayback &) = delete;
  PacedPlayback(PacedPlayback &&) = default;
  PacedPlayback &operator=(PacedPlayback &&) = default;

  void reset(SourcePtr source, bool looping);
  [[nodiscard]] bool valid() const noexcept { return m_source != nullptr; }
  /// Emission over and, for a non-looping clip, the last frame reached.
  [[nodiscard]] bool finished() const noexcept { return m_finished; }

  /// Advances the clock by @p dt seconds and returns the frame to show now, or
  [[nodiscard]] const VideoFrame *advance(f64 dt, i32 *budget = nullptr);

private:
  SourcePtr m_source;
  VideoFrame m_current;
  VideoFrame m_next;
  f64 m_clock = 0.0;
  bool m_has_current = false;
  bool m_has_next = false;
  bool m_looping = false;
  bool m_finished = false;
  bool m_eos = false;
};

} // namespace nxm::video
