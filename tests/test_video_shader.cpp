
#include "framework/nxtest.h"

#include "core/foundation/platform/filesystem.h"
#include "core/rendering/rhi/rhi.h"

#include <cstring>
#include <vector>

namespace {

namespace rhi = nxe::rhi;

constexpr u32 TARGET = 8;

struct VideoPush {
  float rect[4] = {-1.f, -1.f, 1.f, 1.f};
  float uv_scale[2] = {1.f, 1.f};
  float luma_weights[2] = {0.2126f, 0.0722f};
  u32 luma = 0;
  u32 chroma = 0;
  u32 sampler_index = 0;
  u32 full_range = 0;
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

[[nodiscard]] rhi::TextureHandle upload(rhi::Device &device, const rhi::Format fmt,
                                        const u32 width, const u32 height,
                                        const u32 row_pitch,
                                        const std::vector<u8> &data,
                                        const nx::string_view name) {
  const rhi::TextureHandle tex = device.create_texture({
      .name = name,
      .format = fmt,
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
  sub.row_pitch = row_pitch;
  const rhi::ImageSubresource layout[1] = {sub};
  (void)device.uploader().upload_texture(
      tex, std::span<const u8>(data.data(), data.size()),
      std::span<const rhi::ImageSubresource>(layout, 1));
  return tex;
}

[[nodiscard]] rhi::TextureHandle make_luma(rhi::Device &device,
                                           const std::vector<u8> &y) {
  return upload(device, rhi::Format::R8_UNORM, TARGET, TARGET, TARGET, y, "luma");
}

[[nodiscard]] rhi::TextureHandle make_chroma(rhi::Device &device, const u8 cb,
                                             const u8 cr) {
  const u32 cw = TARGET / 2;
  const u32 ch = TARGET / 2;
  std::vector<u8> data(nx::cast<usize>(cw) * ch * 2);
  for (usize i = 0; i < data.size(); i += 2) {
    data[i] = cb;
    data[i + 1] = cr;
  }
  return upload(device, rhi::Format::RG8_UNORM, cw, ch, cw * 2, data, "chroma");
}

struct Rendered {
  rhi::ReadbackResult pixels;
  bool ok = false;
};

[[nodiscard]] Rendered draw(rhi::Device &device, rhi::ShaderHandle shader,
                            rhi::TextureHandle luma, rhi::TextureHandle chroma,
                            rhi::TextureHandle target, rhi::SamplerHandle sampler,
                            const float kr = 0.2126f, const float kb = 0.0722f,
                            const u32 full_range = 0) {
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
  push.luma = device.texture_index(luma);
  push.chroma = device.texture_index(chroma);
  push.sampler_index = device.sampler_index(sampler);
  push.luma_weights[0] = kr;
  push.luma_weights[1] = kb;
  push.full_range = full_range;

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

[[nodiscard]] rhi::TextureHandle make_target(rhi::Device &device) {
  return device.create_texture({
      .name = "video target",
      .format = rhi::Format::RGBA8_UNORM,
      .width = TARGET,
      .height = TARGET,
      .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::CopySrc,
  });
}

[[nodiscard]] usize texel(const u32 row, const u32 col) {
  return (nx::cast<usize>(row) * TARGET + col) * 4;
}

}

TEST_CASE("video shader: a grey frame converts to grey (BT.709)") {
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");
  rhi::Device &device = fixture.device;

  const rhi::ShaderHandle shader = load_video(device);
  if (!shader.valid())
    SKIP("shaders are not built in this configuration");

  const rhi::TextureHandle luma =
      make_luma(device, std::vector<u8>(TARGET * TARGET, 220u));
  const rhi::TextureHandle chroma = make_chroma(device, 128u, 128u);
  device.uploader().flush();
  REQUIRE(luma.valid());
  REQUIRE(chroma.valid());

  const rhi::SamplerHandle sampler = device.create_sampler({.name = "video"});
  REQUIRE(sampler.valid());
  const rhi::TextureHandle target = make_target(device);
  REQUIRE(target.valid());

  const Rendered r = draw(device, shader, luma, chroma, target, sampler);
  REQUIRE(r.ok);

  const usize mid = texel(TARGET / 2, TARGET / 2);
  CHECK(r.pixels.data[mid + 0] > 220u);
  CHECK(r.pixels.data[mid + 1] > 220u);
  CHECK(r.pixels.data[mid + 2] > 220u);
  const int rr = r.pixels.data[mid + 0];
  const int gg = r.pixels.data[mid + 1];
  const int bb = r.pixels.data[mid + 2];
  CHECK(std::abs(rr - gg) <= 2);
  CHECK(std::abs(gg - bb) <= 2);

  device.destroy_texture(target);
  device.destroy_sampler(sampler);
  device.destroy_texture(chroma);
  device.destroy_texture(luma);
  device.destroy_shader(shader);
}

TEST_CASE("video shader: the colour matrix and range change the result") {
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");
  rhi::Device &device = fixture.device;

  const rhi::ShaderHandle shader = load_video(device);
  if (!shader.valid())
    SKIP("shaders are not built in this configuration");

  const rhi::TextureHandle luma =
      make_luma(device, std::vector<u8>(TARGET * TARGET, 128u));
  const rhi::TextureHandle chroma = make_chroma(device, 128u, 180u);
  device.uploader().flush();
  REQUIRE(luma.valid());
  REQUIRE(chroma.valid());

  const rhi::SamplerHandle sampler = device.create_sampler({.name = "video"});
  REQUIRE(sampler.valid());
  const rhi::TextureHandle target = make_target(device);
  REQUIRE(target.valid());

  const usize mid = texel(TARGET / 2, TARGET / 2);

  const Rendered a = draw(device, shader, luma, chroma, target, sampler, 0.2126f,
                          0.0722f, 0);
  REQUIRE(a.ok);
  const int r709 = a.pixels.data[mid + 0];

  const Rendered b = draw(device, shader, luma, chroma, target, sampler, 0.299f,
                          0.114f, 0);
  REQUIRE(b.ok);
  const int r601 = b.pixels.data[mid + 0];

  const Rendered c = draw(device, shader, luma, chroma, target, sampler, 0.2126f,
                          0.0722f, 1);
  REQUIRE(c.ok);
  const int rfull = c.pixels.data[mid + 0];

  CHECK(r709 > r601 + 4);
  CHECK(std::abs(r709 - rfull) > 4);

  device.destroy_texture(target);
  device.destroy_sampler(sampler);
  device.destroy_texture(chroma);
  device.destroy_texture(luma);
  device.destroy_shader(shader);
}

TEST_CASE("video shader: the chroma channels convert, right way up") {
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");
  rhi::Device &device = fixture.device;

  const rhi::ShaderHandle shader = load_video(device);
  if (!shader.valid())
    SKIP("shaders are not built in this configuration");

  std::vector<u8> y(nx::cast<usize>(TARGET) * TARGET, 16u);
  for (u32 row = 0; row < TARGET / 2; ++row)
    for (u32 col = 0; col < TARGET; ++col)
      y[nx::cast<usize>(row) * TARGET + col] = 235u;
  const rhi::TextureHandle luma = make_luma(device, y);
  const rhi::TextureHandle chroma = make_chroma(device, 200u, 128u);
  device.uploader().flush();
  REQUIRE(luma.valid());
  REQUIRE(chroma.valid());

  const rhi::SamplerHandle sampler = device.create_sampler({.name = "video"});
  REQUIRE(sampler.valid());
  const rhi::TextureHandle target = make_target(device);
  REQUIRE(target.valid());

  const Rendered r = draw(device, shader, luma, chroma, target, sampler);
  REQUIRE(r.ok);

  const usize bot = texel(TARGET - 1, TARGET / 2);
  CHECK(int(r.pixels.data[bot + 2]) > int(r.pixels.data[bot + 0]) + 20);
  CHECK(r.pixels.data[texel(0, TARGET / 2) + 1] > 200u);
  CHECK(r.pixels.data[bot + 1] < 40u);

  device.destroy_texture(target);
  device.destroy_sampler(sampler);
  device.destroy_texture(chroma);
  device.destroy_texture(luma);
  device.destroy_shader(shader);
}
