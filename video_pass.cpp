#include "video/video_pass.h"

#include "core/foundation/containers/blob.h"

#include <utility>

namespace nxm::video {
namespace {
namespace rhi = nxe::rhi;
namespace rg = nxe::rg;
} // namespace

void VideoRenderer::draw(rhi::Device &device, rg::RenderGraph &graph,
                         const rg::TextureId target,
                         const rhi::PipelineHandle pipeline,
                         const std::span<const VideoDraw> draws) {
  if (draws.empty() || !pipeline.valid() || !target.valid())
    return;

  nx::vector<VideoDraw> local(draws.begin(), draws.end());
  graph.add_pass(
      "video.draw",
      rg::SetupFn([target](rg::Builder &builder) { builder.color(0, target); }),
      rg::ExecuteFn([pipeline, &device, local = std::move(local)](
                        rhi::CommandContext &cmd, const rg::Resources &) {
        cmd.bind_pipeline(pipeline);
        for (const VideoDraw &d : local) {
          VideoPush push;
          push.rect = d.rect;
          push.uv_scale = d.uv_scale;
          push.luma_weights = luma_coeffs(d.colour.matrix);
          push.luma = device.texture_index(d.luma);
          push.chroma = device.texture_index(d.chroma);
          push.sampler_index = d.sampler_index;
          push.full_range = d.colour.full_range ? 1u : 0u;
          cmd.push_constants(&push, sizeof(push));
          cmd.draw(6);
        }
      }));
}

} // namespace nxm::video
