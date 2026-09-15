#pragma once

#include "video/video_source.h"

#include "rendering/rhi/device.h"

#include "core/foundation/strings/utf8_string_view.h"

#include <glm/vec2.hpp>
#include <memory>

namespace nxm::video {

struct GpuFrame {
  nxe::rhi::TextureHandle luma;
  nxe::rhi::TextureHandle chroma;
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

  [[nodiscard]] virtual bool seek(f64 target_seconds, bool looping) = 0;

  [[nodiscard]] virtual GpuFrame frame_at(f64 target_seconds,
                                          bool looping) = 0;

  [[nodiscard]] virtual f64 position() const noexcept = 0;
  [[nodiscard]] virtual bool finished() const noexcept = 0;
};

using GpuSourcePtr = std::unique_ptr<GpuVideoSource>;

[[nodiscard]] bool hw_decode_available(nxe::rhi::Device &device);

[[nodiscard]] GpuSourcePtr create_video_source(nxe::rhi::Device &device,
                                               nx::string_view path);

}
