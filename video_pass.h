#pragma once

/**
 * @file video_pass.h
 * @brief The video quad, recorded into the frame (namespace nxm::video).
 */

#include "core/rendering/graph/render_graph.h"
#include "core/rendering/rhi/bindless.h"
#include "core/rendering/rhi/descs.h"
#include "core/rendering/rhi/device.h"

#include <glm/vec2.hpp>
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
  glm::vec2 uv_scale{1.f, 1.f};
};
static_assert(sizeof(VideoPush) <= nxe::rhi::PUSH_CONSTANT_SIZE,
              "the video push block must fit the guaranteed push range");

struct VideoDraw {
  glm::vec4 rect{-1.f, -1.f, 1.f, 1.f};
  u32 y_plane = 0;
  u32 cb_plane = 0;
  u32 cr_plane = 0;
  u32 sampler_index = 0;
  glm::vec2 uv_scale{1.f, 1.f};
  bool hw = false;
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
  nxe::rhi::PipelineHandle m_pipeline;      ///< three CPU planes (fs_main)
  nxe::rhi::PipelineHandle m_pipeline_hw;   ///< hardware NV12 (fs_main_nv12)
  nxe::rhi::Format m_format = nxe::rhi::Format::Unknown;
};

} // namespace nxm::video
