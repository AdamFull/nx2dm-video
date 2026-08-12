/**
 * @file test_video_shader.cpp
 * @brief What the video shader does with three YCbCr planes: that it converts
 * to RGB, and that the image is not upside down.
 *
 * The orientation case is the one that cannot be reasoned about safely. Every
 * set_viewport in this engine emits a negative-height Vulkan viewport
 * (vk_resources.cpp), so a full-screen quad's UVs have to account for the flip
 * or the frame renders inverted - and nothing but reading the pixels back
 * catches it. This renders a top-bright/bottom-dark luma ramp and asserts the
 * bright end lands at the top of the target.
 *
 * Skipped rather than failed on a machine with no usable device, like the rest
 * of the rendering suite.
 */

#include "framework/nxtest.h"

#include "core/foundation/platform/filesystem.h"
#include "core/rendering/rhi/rhi.h"

#include <cstring>
#include <vector>

namespace {

namespace rhi = nxe::rhi;

constexpr u32 TARGET = 8;

// Mirrors VideoPush in shaders/video.slang. A plain float[4] rather than a
// glm::vec4 so the test needs nothing but the RHI; the layout is the same
// float4 the shader reads.
struct VideoPush {
  float rect[4] = {-1.f, -1.f, 1.f, 1.f};
  u32 y_plane = 0;
  u32 cb_plane = 0;
  u32 cr_plane = 0;
  u32 sampler_index = 0;
  float uv_scale[2] = {1.f, 1.f};
};

struct TestDevice {
  rhi::Device device;
  bool ready = false;

  TestDevice() {
    rhi::DeviceDesc desc{};
    desc.application_name = "nx video tests";
    ready = device.init(desc);
  }
  ~TestDevice() {
    if (ready)
      device.shutdown();
  }
  TestDevice(const TestDevice &) = delete;
  TestDevice &operator=(const TestDevice &) = delete;
};

[[nodiscard]] rhi::ShaderHandle load_video(rhi::Device &device) {
  const nx::string base = nx::string(NX_TEST_SHADER_DIR) + "/video/video";
  auto code = nx::fs::file_read(nx::fs::path_view(base + ".spv"));
  auto refl = nx::fs::file_read_text(nx::fs::path_view(base + ".refl.json"));
  if (!code.has_value() || !refl.has_value())
    return {};
  return device.create_shader({
      .name = "video",
      .code = code->data(),
      .code_size = code->size(),
      .reflection_json = refl->view(),
  });
}

[[nodiscard]] rhi::TextureHandle make_plane(rhi::Device &device, const u32 width,
                                            const u32 height,
                                            const std::vector<u8> &data,
                                            const nx::string_view name) {
  const rhi::TextureHandle tex = device.create_texture({
      .name = name,
      .format = rhi::Format::R8_UNORM,
      .width = width,
      .height = height,
      .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst,
  });
  if (!tex.valid())
    return {};
  rhi::ImageSubresource sub{};
  sub.size = nx::cast<u64>(data.size());
  sub.width = width;
  sub.height = height;
  sub.depth = 1;
  sub.row_pitch = width;
  const rhi::ImageSubresource layout[1] = {sub};
  (void)device.uploader().upload_texture(
      tex, std::span<const u8>(data.data(), data.size()),
      std::span<const rhi::ImageSubresource>(layout, 1));
  return tex;
}

struct Rendered {
  rhi::ReadbackResult pixels;
  bool ok = false;
};

// Renders one full-screen video quad from the three planes into an RGBA8
// target and reads it back. Leaves all handles for the caller to drop.
[[nodiscard]] Rendered draw_planes(rhi::Device &device, rhi::ShaderHandle shader,
                                   rhi::TextureHandle y, rhi::TextureHandle cb,
                                   rhi::TextureHandle cr,
                                   rhi::TextureHandle target,
                                   rhi::SamplerHandle sampler) {
  const rhi::PipelineHandle pipeline = device.create_graphics_pipeline({
      .name = "video",
      .vertex = {.shader = shader, .entry_point = "vs_main"},
      .fragment = {.shader = shader, .entry_point = "fs_main"},
      .color_formats = {rhi::Format::RGBA8_UNORM},
      .color_count = 1,
  });
  if (!pipeline.valid())
    return {};

  VideoPush push;
  push.y_plane = device.texture_index(y);
  push.cb_plane = device.texture_index(cb);
  push.cr_plane = device.texture_index(cr);
  push.sampler_index = device.sampler_index(sampler);

  rhi::CommandContext cmd;
  if (!device.begin_headless_frame(cmd)) {
    device.destroy_pipeline(pipeline);
    return {};
  }
  cmd.barrier(rhi::TextureBarrier{.texture = target,
                                  .from = rhi::ResourceState::Undefined,
                                  .to = rhi::ResourceState::ColorAttachment});
  rhi::RenderPassDesc pass = {};
  pass.name = "video";
  pass.color[0].texture = target;
  pass.color[0].load = rhi::LoadOp::Clear;
  pass.color[0].store = rhi::StoreOp::Store;
  pass.color[0].clear = rhi::clear_color(0.f, 0.f, 0.f, 1.f);
  pass.color_count = 1;
  cmd.begin_render_pass(pass);
  cmd.set_viewport(
      {.width = nx::cast<f32>(TARGET), .height = nx::cast<f32>(TARGET)});
  cmd.set_scissor({{0, 0}, {TARGET, TARGET}});
  cmd.bind_pipeline(pipeline);
  cmd.push_constants(&push, sizeof(push));
  cmd.draw(6, 1);
  cmd.end_render_pass();
  cmd.barrier(rhi::TextureBarrier{.texture = target,
                                  .from = rhi::ResourceState::ColorAttachment,
                                  .to = rhi::ResourceState::CopySrc});
  if (!device.end_headless_frame()) {
    device.destroy_pipeline(pipeline);
    return {};
  }
  device.wait_idle();

  Rendered out;
  out.pixels = device.uploader().read_texture(target);
  out.ok = out.pixels.data != nullptr;
  if (out.ok)
    device.uploader().wait(out.pixels.ticket);
  device.destroy_pipeline(pipeline);
  return out;
}

// A two-component (RG8) texture, for the interleaved NV12 chroma plane.
[[nodiscard]] rhi::TextureHandle make_rg8(rhi::Device &device, const u32 width,
                                          const u32 height,
                                          const std::vector<u8> &data,
                                          const nx::string_view name) {
  const rhi::TextureHandle tex = device.create_texture({
      .name = name,
      .format = rhi::Format::RG8_UNORM,
      .width = width,
      .height = height,
      .usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst,
  });
  if (!tex.valid())
    return {};
  rhi::ImageSubresource sub{};
  sub.size = nx::cast<u64>(data.size());
  sub.width = width;
  sub.height = height;
  sub.depth = 1;
  sub.row_pitch = width * 2;
  const rhi::ImageSubresource layout[1] = {sub};
  (void)device.uploader().upload_texture(
      tex, std::span<const u8>(data.data(), data.size()),
      std::span<const rhi::ImageSubresource>(layout, 1));
  return tex;
}

[[nodiscard]] Rendered draw_nv12(rhi::Device &device, rhi::ShaderHandle shader,
                                 rhi::TextureHandle luma,
                                 rhi::TextureHandle chroma,
                                 rhi::TextureHandle target,
                                 rhi::SamplerHandle sampler) {
  const rhi::PipelineHandle pipeline = device.create_graphics_pipeline({
      .name = "video.nv12",
      .vertex = {.shader = shader, .entry_point = "vs_main"},
      .fragment = {.shader = shader, .entry_point = "fs_main_nv12"},
      .color_formats = {rhi::Format::RGBA8_UNORM},
      .color_count = 1,
  });
  if (!pipeline.valid())
    return {};

  VideoPush push;
  push.y_plane = device.texture_index(luma);
  push.cb_plane = device.texture_index(chroma);
  push.sampler_index = device.sampler_index(sampler);

  rhi::CommandContext cmd;
  if (!device.begin_headless_frame(cmd)) {
    device.destroy_pipeline(pipeline);
    return {};
  }
  cmd.barrier(rhi::TextureBarrier{.texture = target,
                                  .from = rhi::ResourceState::Undefined,
                                  .to = rhi::ResourceState::ColorAttachment});
  rhi::RenderPassDesc pass = {};
  pass.name = "video.nv12";
  pass.color[0].texture = target;
  pass.color[0].load = rhi::LoadOp::Clear;
  pass.color[0].store = rhi::StoreOp::Store;
  pass.color[0].clear = rhi::clear_color(0.f, 0.f, 0.f, 1.f);
  pass.color_count = 1;
  cmd.begin_render_pass(pass);
  cmd.set_viewport(
      {.width = nx::cast<f32>(TARGET), .height = nx::cast<f32>(TARGET)});
  cmd.set_scissor({{0, 0}, {TARGET, TARGET}});
  cmd.bind_pipeline(pipeline);
  cmd.push_constants(&push, sizeof(push));
  cmd.draw(6, 1);
  cmd.end_render_pass();
  cmd.barrier(rhi::TextureBarrier{.texture = target,
                                  .from = rhi::ResourceState::ColorAttachment,
                                  .to = rhi::ResourceState::CopySrc});
  if (!device.end_headless_frame()) {
    device.destroy_pipeline(pipeline);
    return {};
  }
  device.wait_idle();

  Rendered out;
  out.pixels = device.uploader().read_texture(target);
  out.ok = out.pixels.data != nullptr;
  if (out.ok)
    device.uploader().wait(out.pixels.ticket);
  device.destroy_pipeline(pipeline);
  return out;
}

[[nodiscard]] usize texel(const u32 row, const u32 col) {
  return (nx::cast<usize>(row) * TARGET + col) * 4;
}

} // namespace

TEST_CASE("video shader: a grey frame converts to grey (BT.709)") {
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");
  rhi::Device &device = fixture.device;

  const rhi::ShaderHandle shader = load_video(device);
  if (!shader.valid())
    SKIP("shaders are not built in this configuration");

  // Y = 220 everywhere, chroma neutral: a bright grey. Limited-range 709 puts
  // it near (220-16)*255/219 = 237 on every channel, with no colour cast.
  const std::vector<u8> y(TARGET * TARGET, 220u);
  const std::vector<u8> c((TARGET / 2) * (TARGET / 2), 128u);
  const rhi::TextureHandle yt = make_plane(device, TARGET, TARGET, y, "y");
  const rhi::TextureHandle cbt =
      make_plane(device, TARGET / 2, TARGET / 2, c, "cb");
  const rhi::TextureHandle crt =
      make_plane(device, TARGET / 2, TARGET / 2, c, "cr");
  device.uploader().flush();
  REQUIRE(yt.valid());
  REQUIRE(cbt.valid());
  REQUIRE(crt.valid());

  const rhi::SamplerHandle sampler = device.create_sampler({.name = "video"});
  REQUIRE(sampler.valid());
  const rhi::TextureHandle target = device.create_texture({
      .name = "video target",
      .format = rhi::Format::RGBA8_UNORM,
      .width = TARGET,
      .height = TARGET,
      .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc,
  });
  REQUIRE(target.valid());

  const Rendered r =
      draw_planes(device, shader, yt, cbt, crt, target, sampler);
  REQUIRE(r.ok);

  const usize mid = texel(TARGET / 2, TARGET / 2);
  CHECK(r.pixels.data[mid + 0] > 220u);
  CHECK(r.pixels.data[mid + 1] > 220u);
  CHECK(r.pixels.data[mid + 2] > 220u);
  // Neutral chroma means the channels agree.
  const int rr = r.pixels.data[mid + 0];
  const int gg = r.pixels.data[mid + 1];
  const int bb = r.pixels.data[mid + 2];
  CHECK(std::abs(rr - gg) <= 2);
  CHECK(std::abs(gg - bb) <= 2);

  device.destroy_texture(target);
  device.destroy_sampler(sampler);
  device.destroy_texture(crt);
  device.destroy_texture(cbt);
  device.destroy_texture(yt);
  device.destroy_shader(shader);
}

TEST_CASE("video shader: the NV12 path converts and lands right way up") {
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");
  rhi::Device &device = fixture.device;

  const rhi::ShaderHandle shader = load_video(device);
  if (!shader.valid())
    SKIP("shaders are not built in this configuration");

  std::vector<u8> luma(nx::cast<usize>(TARGET) * TARGET, 16u);
  for (u32 row = 0; row < TARGET / 2; ++row)
    for (u32 col = 0; col < TARGET; ++col)
      luma[nx::cast<usize>(row) * TARGET + col] = 235u;
  std::vector<u8> chroma(nx::cast<usize>(TARGET / 2) * (TARGET / 2) * 2, 128u);
  for (usize i = 0; i < chroma.size(); i += 2)
    chroma[i] = 200u; // Cb high -> blue push; Cr stays neutral

  const rhi::TextureHandle yt = make_plane(device, TARGET, TARGET, luma, "y");
  const rhi::TextureHandle ct =
      make_rg8(device, TARGET / 2, TARGET / 2, chroma, "cbcr");
  device.uploader().flush();
  REQUIRE(yt.valid());
  REQUIRE(ct.valid());

  const rhi::SamplerHandle sampler = device.create_sampler({.name = "video"});
  REQUIRE(sampler.valid());
  const rhi::TextureHandle target = device.create_texture({
      .name = "video target",
      .format = rhi::Format::RGBA8_UNORM,
      .width = TARGET,
      .height = TARGET,
      .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc,
  });
  REQUIRE(target.valid());

  const Rendered r = draw_nv12(device, shader, yt, ct, target, sampler);
  REQUIRE(r.ok);

  // In the dark-luma bottom, where chroma shows, high Cb pushes blue above red;
  // a Cb/Cr swap would push red instead.
  const usize bot = texel(TARGET - 1, TARGET / 2);
  CHECK(int(r.pixels.data[bot + 2]) > int(r.pixels.data[bot + 0]) + 20);
  // Green tracks luma (Cb barely touches it): bright top, dark bottom - upright.
  CHECK(r.pixels.data[texel(0, TARGET / 2) + 1] > 200u);
  CHECK(r.pixels.data[bot + 1] < 40u);

  device.destroy_texture(target);
  device.destroy_sampler(sampler);
  device.destroy_texture(ct);
  device.destroy_texture(yt);
  device.destroy_shader(shader);
}

TEST_CASE("video shader: the top of the image lands at the top of the frame") {
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");
  rhi::Device &device = fixture.device;

  const rhi::ShaderHandle shader = load_video(device);
  if (!shader.valid())
    SKIP("shaders are not built in this configuration");

  // Top half of the image bright, bottom half dark; chroma neutral. Correctly
  // oriented, the bright end reads back at row 0 - the top of the target.
  std::vector<u8> y(TARGET * TARGET, 16u);
  for (u32 row = 0; row < TARGET / 2; ++row)
    for (u32 col = 0; col < TARGET; ++col)
      y[nx::cast<usize>(row) * TARGET + col] = 235u;
  const std::vector<u8> c((TARGET / 2) * (TARGET / 2), 128u);

  const rhi::TextureHandle yt = make_plane(device, TARGET, TARGET, y, "y");
  const rhi::TextureHandle cbt =
      make_plane(device, TARGET / 2, TARGET / 2, c, "cb");
  const rhi::TextureHandle crt =
      make_plane(device, TARGET / 2, TARGET / 2, c, "cr");
  device.uploader().flush();
  REQUIRE(yt.valid());

  const rhi::SamplerHandle sampler = device.create_sampler({.name = "video"});
  REQUIRE(sampler.valid());
  const rhi::TextureHandle target = device.create_texture({
      .name = "video target",
      .format = rhi::Format::RGBA8_UNORM,
      .width = TARGET,
      .height = TARGET,
      .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc,
  });
  REQUIRE(target.valid());

  const Rendered r =
      draw_planes(device, shader, yt, cbt, crt, target, sampler);
  REQUIRE(r.ok);

  const int top = r.pixels.data[texel(0, TARGET / 2) + 1];
  const int bottom = r.pixels.data[texel(TARGET - 1, TARGET / 2) + 1];
  CHECK(top > 200);   // image top is bright
  CHECK(bottom < 40); // image bottom is dark

  device.destroy_texture(target);
  device.destroy_sampler(sampler);
  device.destroy_texture(crt);
  device.destroy_texture(cbt);
  device.destroy_texture(yt);
  device.destroy_shader(shader);
}
