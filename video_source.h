#pragma once

#include "video/video_asset.h"

#include "core/foundation/containers/blob.h"
#include "core/foundation/core/foundation.h"

#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>

namespace nxm::video {

inline constexpr i32 MAX_VIDEO_DECODE_STEPS_PER_ADVANCE = 256;

enum class ColourMatrix : u8 { BT601, BT709, BT2020 };

struct ColourInfo {
  ColourMatrix matrix = ColourMatrix::BT709;
  bool full_range = false;
};

struct VideoFrame {
  u32 width = 0;
  u32 height = 0;
  u32 y_pitch = 0;
  u32 c_pitch = 0;
  f64 pts = 0.0;
  ColourInfo colour;
  nx::vector<u8> y;
  nx::vector<u8> cb;
  nx::vector<u8> cr;

  [[nodiscard]] u32 chroma_width() const noexcept {
    return width / 2 + width % 2;
  }
  [[nodiscard]] u32 chroma_height() const noexcept {
    return height / 2 + height % 2;
  }
};

[[nodiscard]] bool valid_video_dimensions(u64 width, u64 height) noexcept;

[[nodiscard]] bool valid_video_frame(const VideoFrame &frame) noexcept;

[[nodiscard]] bool fill_i420(VideoFrame &out, u32 width, u32 height,
                             u32 chroma_width, u32 chroma_height, f64 pts,
                             const u8 *y, ptrdiff_t y_stride, const u8 *cb,
                             ptrdiff_t cb_stride, const u8 *cr,
                             ptrdiff_t cr_stride) noexcept;

bool interleave_chroma(const VideoFrame &frame, nx::vector<u8> &out) noexcept;

class FrameSource {
public:
  virtual ~FrameSource() = default;

  [[nodiscard]] virtual u32 width() const noexcept = 0;
  [[nodiscard]] virtual u32 height() const noexcept = 0;
  [[nodiscard]] virtual f64 frame_rate() const noexcept = 0;
  [[nodiscard]] virtual f64 duration() const noexcept { return 0.0; }

  [[nodiscard]] virtual bool next(VideoFrame &out) = 0;

  virtual void restart() = 0;

  [[nodiscard]] virtual bool seek(f64 target_seconds) {
    (void)target_seconds;
    return false;
  }
};

/// std::unique_ptr rather than nx::unique_ptr, deliberately: it owns an
/// interface, which AllocDeleter cannot. Same reason the audio DecoderPtr does.
using SourcePtr = std::unique_ptr<FrameSource>;

class SyntheticSource final : public FrameSource {
public:
  SyntheticSource(u32 width, u32 height, f64 fps) noexcept;

  [[nodiscard]] u32 width() const noexcept override { return m_width; }
  [[nodiscard]] u32 height() const noexcept override { return m_height; }
  [[nodiscard]] f64 frame_rate() const noexcept override { return m_fps; }

  [[nodiscard]] bool next(VideoFrame &out) override;
  void restart() override { m_frame = 0; }
  [[nodiscard]] bool seek(f64 target_seconds) noexcept override {
    if (!std::isfinite(target_seconds) || target_seconds <= 0.0) {
      m_frame = 0;
    } else {
      const f64 largest =
          nx::cast<f64>(std::numeric_limits<u64>::max()) / m_fps;
      m_frame = target_seconds >= largest
                    ? std::numeric_limits<u64>::max()
                    : nx::cast<u64>(target_seconds * m_fps);
    }
    return true;
  }

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
  void set_looping(bool looping) noexcept { m_looping = looping; }
  [[nodiscard]] bool valid() const noexcept { return m_source != nullptr; }
  [[nodiscard]] f64 duration() const noexcept {
    return m_source != nullptr ? m_source->duration() : 0.0;
  }
  [[nodiscard]] bool finished() const noexcept { return m_finished; }

  [[nodiscard]] const VideoFrame *advance(f64 dt, i32 *budget = nullptr);

  [[nodiscard]] const VideoFrame *advance_to(f64 target, i32 *budget = nullptr);

  [[nodiscard]] bool seek(f64 target_seconds);
  [[nodiscard]] bool seek(f64 source_seconds, f64 presentation_seconds);

private:
  SourcePtr m_source;
  VideoFrame m_current;
  VideoFrame m_next;
  // Presentation time is monotonic across loops; m_clock is local to the
  // current pass through the clip and resets when the source restarts.
  f64 m_presentation_clock = 0.0;
  f64 m_clock = 0.0;
  bool m_has_current = false;
  bool m_has_next = false;
  bool m_looping = false;
  bool m_finished = false;
  bool m_eos = false;
};

} // namespace nxm::video
