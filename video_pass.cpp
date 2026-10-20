#include "video/video_pass.h"

#include "core/foundation/containers/blob.h"
#include "rendering/rhi/shaders/nx_interop.h"

#include <utility>

namespace gfx = nx::gfx;

namespace nxm::video {
namespace {
namespace rg = nxe::rg;
} // namespace

void VideoRenderer::draw(gfx::device &device, rg::RenderGraph &graph,
                         const rg::TextureId target,
                         const gfx::pipeline_handle pipeline,
                         const std::span<const VideoDraw> draws) {
  if (draws.empty() || !pipeline.valid() || !target.valid())
    return;

  nx::vector<VideoDraw> local(draws.begin(), draws.end());
  graph.add_pass(
      "video.draw",
      rg::SetupFn([target](rg::Builder &builder) { builder.color(0, target); }),
      rg::ExecuteFn([pipeline, &device, local = std::move(local)](
                        gfx::command_list &cmd, const rg::Resources &) {
        gfx::bind_pipeline(cmd, pipeline);
        for (const VideoDraw &d : local) {
          GpuVideoPush push;
          push.rect = d.rect;
          push.uv_scale = d.uv_scale;
          push.luma_weights = luma_coeffs(d.colour.matrix);
          push.luma = NxTexture2D<float4>::from_indices(
              gfx::texture_index(device, d.luma), d.sampler_index);
          push.chroma = NxTexture2D<float4>::from_indices(
              gfx::texture_index(device, d.chroma), d.sampler_index);
          push.full_range = d.colour.full_range ? 1u : 0u;
          gfx::push_constants(cmd, &push, sizeof(push));
          gfx::draw(cmd, 6);
        }
      }));
}

} // namespace nxm::video
