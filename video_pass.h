#pragma once

#include "video/video_interop.h"
#include "video/video_source.h"

#include "nx/gfx/bindless.h"
#include "nx/gfx/gfx.h"
#include "rendering/graph/render_graph.h"
#include "rendering/rhi/shaders/nx_texture.hpp"

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <span>

namespace nxm::video {

static_assert(sizeof(GpuVideoPush) <= nx::gfx::PUSH_CONSTANT_SIZE,
              "the video push block must fit the guaranteed push range");

struct VideoDraw {
  glm::vec4 rect{-1.f, -1.f, 1.f, 1.f};
  glm::vec2 uv_scale{1.f, 1.f};
  nx::gfx::texture_handle luma;
  nx::gfx::texture_handle chroma;
  u32 sampler_index = 0;
  ColourInfo colour;
};

[[nodiscard]] inline glm::vec2 luma_coeffs(const ColourMatrix matrix) noexcept {
  switch (matrix) {
  case ColourMatrix::BT601:
    return {0.299f, 0.114f};
  case ColourMatrix::BT2020:
    return {0.2627f, 0.0593f};
  case ColourMatrix::BT709:
    break;
  }
  return {0.2126f, 0.0722f};
}

class VideoRenderer {
public:
  VideoRenderer() = default;
  ~VideoRenderer() = default;
  VideoRenderer(const VideoRenderer &) = delete;
  VideoRenderer &operator=(const VideoRenderer &) = delete;

  /// Records with a runtime-owned pipeline. The renderer does not retain or
  /// destroy it.
  void draw(nx::gfx::device &device, nxe::rg::RenderGraph &graph,
            nxe::rg::TextureId target, nx::gfx::pipeline_handle pipeline,
            std::span<const VideoDraw> draws);
};

} // namespace nxm::video
