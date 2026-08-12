#pragma once

/**
 * @file video_hw.h
 * @brief The hardware-decode clip (namespace nxm::video).
 */

#include "core/rendering/rhi/device.h"

#include "core/foundation/strings/utf8_string_view.h"

#include <glm/vec2.hpp>
#include <memory>

namespace nxm::video {

/// Whether the device can hardware-decode the VP9 this module plays.
[[nodiscard]] bool hw_decode_available(nxe::rhi::Device &device);

class HwVideoSource {
public:
  HwVideoSource();
  ~HwVideoSource();
  HwVideoSource(const HwVideoSource &) = delete;
  HwVideoSource &operator=(const HwVideoSource &) = delete;

  [[nodiscard]] bool open(nxe::rhi::Device &device, nx::string_view path);
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] u32 width() const noexcept;
  [[nodiscard]] u32 height() const noexcept;
  [[nodiscard]] f64 frame_rate() const noexcept;

  [[nodiscard]] nxe::rhi::TextureHandle
  frame_at(f64 target_seconds, bool looping, glm::vec2 &uv_scale);

  /// PTS of the frame currently held, in seconds.
  [[nodiscard]] f64 position() const noexcept;
  /// True once a non-looping clip has shown its last frame.
  [[nodiscard]] bool finished() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};

} // namespace nxm::video
