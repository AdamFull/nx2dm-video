#pragma once

/**
 * @file video_pass.h
 * @brief The video quad, recorded into the frame (namespace nxm::video).
 */

#include "core/rendering/graph/render_graph.h"
#include "core/rendering/rhi/bindless.h"
#include "core/rendering/rhi/descs.h"
#include "core/rendering/rhi/device.h"

#include <glm/vec4.hpp>
#include <span>

namespace nxm::video {

/// Mirrors VideoPush in shaders/video.slang: a vector then scalars, so neither
/// side has padding to guess.
struct VideoPush {
  glm::vec4 rect{-1.f, -1.f, 1.f, 1.f};
  u32 y_plane = 0;
  u32 cb_plane = 0;
  u32 cr_plane = 0;
  u32 sampler_index = 0;
};
static_assert(sizeof(VideoPush) <= nxe::rhi::PUSH_CONSTANT_SIZE,
              "the video push block must fit the guaranteed push range");

/// One quad: its destination rect in NDC and the three plane slots, already
/// resolved to bindless indices.
struct VideoDraw {
  glm::vec4 rect{-1.f, -1.f, 1.f, 1.f};
  u32 y_plane = 0;
  u32 cb_plane = 0;
  u32 cr_plane = 0;
  u32 sampler_index = 0;
};

class VideoRenderer {
public:
  VideoRenderer() = default;
  ~VideoRenderer() = default;
  VideoRenderer(const VideoRenderer &) = delete;
  VideoRenderer &operator=(const VideoRenderer &) = delete;

  [[nodiscard]] bool init(nxe::rhi::Device &device,
                          nxe::rhi::ShaderHandle shader);
  void shutdown(nxe::rhi::Device &device);
  [[nodiscard]] bool ready() const noexcept { return m_shader.valid(); }

  void draw(nxe::rhi::Device &device, nxe::rg::RenderGraph &graph,
            nxe::rg::TextureId target, nxe::rhi::Format format,
            std::span<const VideoDraw> draws);

private:
  [[nodiscard]] bool ensure_pipeline(nxe::rhi::Device &device,
                                     nxe::rhi::Format format);

  nxe::rhi::ShaderHandle m_shader;
  nxe::rhi::PipelineHandle m_pipeline;
  nxe::rhi::Format m_format = nxe::rhi::Format::Unknown;
};

} // namespace nxm::video
