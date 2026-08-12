#pragma once

/**
 * @file video_gpu.h
 * @brief A decoded clip as GPU plane textures, and the one interface every
 * decode backend implements (namespace nxm::video).
 */

#include "video/video_source.h"

#include "core/rendering/rhi/device.h"

#include "core/foundation/strings/utf8_string_view.h"

#include <glm/vec2.hpp>
#include <memory>

namespace nxm::video {

struct GpuFrame {
  nxe::rhi::TextureHandle luma;   ///< R8, full resolution
  nxe::rhi::TextureHandle chroma; ///< R8G8 interleaved Cb,Cr, half resolution
  glm::vec2 uv_scale{1.f, 1.f};
  ColourInfo colour;
  [[nodiscard]] bool valid() const noexcept {
    return luma.valid() && chroma.valid();
  }
};

class GpuVideoSource {
public:
  virtual ~GpuVideoSource() = default;

  [[nodiscard]] virtual bool open(nxe::rhi::Device &device,
                                  nx::string_view path) = 0;
  [[nodiscard]] virtual bool valid() const noexcept = 0;
  [[nodiscard]] virtual u32 width() const noexcept = 0;
  [[nodiscard]] virtual u32 height() const noexcept = 0;
  [[nodiscard]] virtual f64 frame_rate() const noexcept = 0;

  /// Decodes up to @p target_seconds and returns the frame to show, looping if
  /// @p looping and the clip has ended.
  [[nodiscard]] virtual GpuFrame frame_at(f64 target_seconds,
                                          bool looping) = 0;

  /// The show-time of the frame currently held, in seconds.
  [[nodiscard]] virtual f64 position() const noexcept = 0;
  /// True once a non-looping clip has shown its last frame.
  [[nodiscard]] virtual bool finished() const noexcept = 0;
};

using GpuSourcePtr = std::unique_ptr<GpuVideoSource>;

/// Whether the device can hardware-decode the VP9 the module plays.
[[nodiscard]] bool hw_decode_available(nxe::rhi::Device &device);

[[nodiscard]] GpuSourcePtr create_video_source(nxe::rhi::Device &device,
                                               nx::string_view path);

} // namespace nxm::video
