#include "video/video_pass.h"

#include "core/foundation/containers/blob.h"

#include <utility>

namespace nxm::video {
namespace {
namespace rhi = nxe::rhi;
namespace rg = nxe::rg;
} // namespace

bool VideoRenderer::init(rhi::Device &, const rhi::ShaderHandle shader) {
  if (!shader.valid())
    return false;
  m_shader = shader;
  return true;
}

void VideoRenderer::shutdown(rhi::Device &device) {
  if (m_pipeline.valid())
    device.destroy_pipeline(m_pipeline);
  m_pipeline = {};
  if (m_pipeline_rgba.valid())
    device.destroy_pipeline(m_pipeline_rgba);
  m_pipeline_rgba = {};
  if (m_shader.valid())
    device.destroy_shader(m_shader);
  m_shader = {};
  m_format = rhi::Format::Unknown;
}

bool VideoRenderer::ensure_pipeline(rhi::Device &device,
                                    const rhi::Format format) {
  if (format == m_format && m_pipeline.valid())
    return true;

  // The scene target's format is not known until the first frame and can change
  // with the swapchain. Rebuilding is rare enough to be simple.
  if (m_pipeline.valid())
    device.destroy_pipeline(m_pipeline);
  if (m_pipeline_rgba.valid())
    device.destroy_pipeline(m_pipeline_rgba);

  m_pipeline = device.create_graphics_pipeline({
      .name = "video",
      .vertex = {.shader = m_shader, .entry_point = "vs_main"},
      .fragment = {.shader = m_shader, .entry_point = "fs_main"},
      .color_formats = {format},
      .color_count = 1,
  });
  m_pipeline_rgba = device.create_graphics_pipeline({
      .name = "video.rgba",
      .vertex = {.shader = m_shader, .entry_point = "vs_main"},
      .fragment = {.shader = m_shader, .entry_point = "fs_main_rgba"},
      .color_formats = {format},
      .color_count = 1,
  });
  if (!m_pipeline.valid() || !m_pipeline_rgba.valid())
    return false;
  m_format = format;
  return true;
}

void VideoRenderer::draw(rhi::Device &device, rg::RenderGraph &graph,
                         const rg::TextureId target, const rhi::Format format,
                         const std::span<const VideoDraw> draws) {
  if (draws.empty() || !ready() || !target.valid())
    return;
  if (!ensure_pipeline(device, format))
    return;

  // Copied into the pass, not referenced: the caller rebuilds its list next
  // frame, before this pass has executed.
  nx::vector<VideoDraw> local(draws.begin(), draws.end());
  graph.add_pass(
      "video.draw",
      rg::SetupFn([target](rg::Builder &builder) { builder.color(0, target); }),
      rg::ExecuteFn([this, local = std::move(local)](
                        rhi::CommandContext &cmd, const rg::Resources &) {
        bool rgba_bound = false;
        bool any_bound = false;
        for (const VideoDraw &d : local) {
          if (!any_bound || d.rgba != rgba_bound) {
            cmd.bind_pipeline(d.rgba ? m_pipeline_rgba : m_pipeline);
            rgba_bound = d.rgba;
            any_bound = true;
          }
          VideoPush push;
          push.rect = d.rect;
          push.y_plane = d.y_plane;
          push.cb_plane = d.cb_plane;
          push.cr_plane = d.cr_plane;
          push.sampler_index = d.sampler_index;
          push.uv_scale = d.uv_scale;
          cmd.push_constants(&push, sizeof(push));
          cmd.draw(6);
        }
      }));
}

} // namespace nxm::video
