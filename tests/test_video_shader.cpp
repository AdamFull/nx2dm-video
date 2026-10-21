
#include "framework/nxtest.h"

#include "core/foundation/platform/filesystem.h"
#include "nx/gfx/gfx.h"
#include "rendering/interop/nx_interop.h"
#include "rendering/owned_device.h"
#include "rendering/pipeline_test_utils.h"
#include "video/video_interop.h"

#include <cstring>
#include <vector>

namespace gfx = nx::gfx;

namespace {

constexpr u32 TARGET = 8;

struct TestDevice {
  nxe::test::OwnedDevice device;
  bool ready = false;

  TestDevice() {
    gfx::device_desc desc{};
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

[[nodiscard]] gfx::shader_handle load_video(gfx::device &device) {
  const nx::string base = nx::string(NX_TEST_SHADER_DIR) + "/video/video";
  auto code = nx::fs::file_read(nx::fs::path_view(base + ".spv"));
  auto refl = nx::fs::file_read_text(nx::fs::path_view(base + ".refl.json"));
  if (!code.has_value() || !refl.has_value())
    return {};
  return nxe::test::create_shader(
      device,
      {.name = "video", .code = code->data(), .code_size = code->size()},
      refl->view());
}

[[nodiscard]] gfx::texture_handle upload(gfx::device &device,
                                         const gfx::format fmt, const u32 width,
                                         const u32 height, const u32 row_pitch,
                                         const std::vector<u8> &data,
                                         const nx::string_view name) {
  const gfx::texture_handle tex =
      gfx::create_texture(device, {
                                      .name = name,
                                      .format = fmt,
                                      .width = width,
                                      .height = height,
                                      .usage = gfx::texture_usage::Sampled |
                                               gfx::texture_usage::CopyDst,
                                  });
  if (!tex.valid())
    return {};
  gfx::image_subresource sub{};
  sub.size = nx::cast<u64>(data.size());
  sub.width = width;
  sub.height = height;
  sub.depth = 1;
  sub.row_pitch = row_pitch;
  const gfx::image_subresource layout[1] = {sub};
  (void)gfx::upload_texture(device, tex,
                            std::span<const u8>(data.data(), data.size()),
                            std::span<const gfx::image_subresource>(layout, 1));
  return tex;
}

[[nodiscard]] gfx::texture_handle make_luma(gfx::device &device,
                                            const std::vector<u8> &y) {
  return upload(device, gfx::format::R8_UNORM, TARGET, TARGET, TARGET, y,
                "luma");
}

[[nodiscard]] gfx::texture_handle make_chroma(gfx::device &device, const u8 cb,
                                              const u8 cr) {
  const u32 cw = TARGET / 2;
  const u32 ch = TARGET / 2;
  std::vector<u8> data(nx::cast<usize>(cw) * ch * 2);
  for (usize i = 0; i < data.size(); i += 2) {
    data[i] = cb;
    data[i + 1] = cr;
  }
  return upload(device, gfx::format::RG8_UNORM, cw, ch, cw * 2, data, "chroma");
}

struct Rendered {
  gfx::readback_result pixels;
  bool ok = false;
};

[[nodiscard]] Rendered
draw(gfx::device &device, gfx::shader_handle shader, gfx::texture_handle luma,
     gfx::texture_handle chroma, gfx::texture_handle target,
     gfx::sampler_handle sampler, const float kr = 0.2126f,
     const float kb = 0.0722f, const u32 full_range = 0) {
  const gfx::pipeline_handle pipeline = nxe::test::compile_pipeline(
      device, {
                  .name = "video",
                  .vertex = {.shader = shader, .entry_point = "vs_main"},
                  .fragment = {.shader = shader, .entry_point = "fs_main"},
                  .color_formats = {gfx::format::RGBA8_UNORM},
                  .color_count = 1,
              });
  if (!pipeline.valid())
    return {};

  GpuVideoPush push;
  const u32 sampler_index = gfx::sampler_index(device, sampler);
  push.luma = NxTexture2D<float4>::from_indices(
      gfx::texture_index(device, luma), sampler_index);
  push.chroma = NxTexture2D<float4>::from_indices(
      gfx::texture_index(device, chroma), sampler_index);
  push.luma_weights[0] = kr;
  push.luma_weights[1] = kb;
  push.full_range = full_range;

  gfx::command_list *cmd = nullptr;
  if ((cmd = gfx::begin_headless_frame(device)) == nullptr) {
    gfx::destroy_pipeline(device, pipeline);
    return {};
  }
  gfx::barrier(
      *cmd, gfx::texture_barrier{.texture = target,
                                 .from = gfx::resource_state::Undefined,
                                 .to = gfx::resource_state::ColorAttachment});
  gfx::render_pass_desc pass = {};
  pass.name = "video";
  pass.color[0].texture = target;
  pass.color[0].load = gfx::load_op::Clear;
  pass.color[0].store = gfx::store_op::Store;
  pass.color[0].clear = gfx::clear_color(0.f, 0.f, 0.f, 1.f);
  pass.color_count = 1;
  gfx::begin_render_pass(*cmd, pass);
  gfx::set_viewport(
      *cmd, {.width = nx::cast<f32>(TARGET), .height = nx::cast<f32>(TARGET)});
  gfx::set_scissor(*cmd, {{0, 0}, {TARGET, TARGET}});
  gfx::bind_pipeline(*cmd, pipeline);
  gfx::push_constants(*cmd, &push, sizeof(push));
  gfx::draw(*cmd, 6, 1);
  gfx::end_render_pass(*cmd);
  gfx::barrier(
      *cmd, gfx::texture_barrier{.texture = target,
                                 .from = gfx::resource_state::ColorAttachment,
                                 .to = gfx::resource_state::CopySrc});
  if (!gfx::end_headless_frame(device)) {
    gfx::destroy_pipeline(device, pipeline);
    return {};
  }
  gfx::wait_idle(device);

  Rendered out;
  out.pixels = gfx::read_texture(device, target);
  out.ok = out.pixels.data != nullptr;
  if (out.ok)
    gfx::wait_upload(device, out.pixels.ticket);
  gfx::destroy_pipeline(device, pipeline);
  return out;
}

[[nodiscard]] gfx::texture_handle make_target(gfx::device &device) {
  return gfx::create_texture(device,
                             {
                                 .name = "video target",
                                 .format = gfx::format::RGBA8_UNORM,
                                 .width = TARGET,
                                 .height = TARGET,
                                 .usage = gfx::texture_usage::RenderTarget |
                                          gfx::texture_usage::CopySrc,
                             });
}

[[nodiscard]] usize texel(const u32 row, const u32 col) {
  return (nx::cast<usize>(row) * TARGET + col) * 4;
}

} // namespace

TEST_CASE("video shader: a grey frame converts to grey (BT.709)") {
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");
  gfx::device &device = fixture.device;

  const gfx::shader_handle shader = load_video(device);
  if (!shader.valid())
    SKIP("shaders are not built in this configuration");

  const gfx::texture_handle luma =
      make_luma(device, std::vector<u8>(TARGET * TARGET, 220u));
  const gfx::texture_handle chroma = make_chroma(device, 128u, 128u);
  gfx::wait_uploads(device);
  REQUIRE(luma.valid());
  REQUIRE(chroma.valid());

  const gfx::sampler_handle sampler =
      gfx::create_sampler(device, {.name = "video"});
  REQUIRE(sampler.valid());
  const gfx::texture_handle target = make_target(device);
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

  gfx::destroy_texture(device, target);
  gfx::destroy_sampler(device, sampler);
  gfx::destroy_texture(device, chroma);
  gfx::destroy_texture(device, luma);
  gfx::destroy_shader(device, shader);
}

TEST_CASE("video shader: the colour matrix and range change the result") {
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");
  gfx::device &device = fixture.device;

  const gfx::shader_handle shader = load_video(device);
  if (!shader.valid())
    SKIP("shaders are not built in this configuration");

  const gfx::texture_handle luma =
      make_luma(device, std::vector<u8>(TARGET * TARGET, 128u));
  const gfx::texture_handle chroma = make_chroma(device, 128u, 180u);
  gfx::wait_uploads(device);
  REQUIRE(luma.valid());
  REQUIRE(chroma.valid());

  const gfx::sampler_handle sampler =
      gfx::create_sampler(device, {.name = "video"});
  REQUIRE(sampler.valid());
  const gfx::texture_handle target = make_target(device);
  REQUIRE(target.valid());

  const usize mid = texel(TARGET / 2, TARGET / 2);

  const Rendered a =
      draw(device, shader, luma, chroma, target, sampler, 0.2126f, 0.0722f, 0);
  REQUIRE(a.ok);
  const int r709 = a.pixels.data[mid + 0];

  const Rendered b =
      draw(device, shader, luma, chroma, target, sampler, 0.299f, 0.114f, 0);
  REQUIRE(b.ok);
  const int r601 = b.pixels.data[mid + 0];

  const Rendered c =
      draw(device, shader, luma, chroma, target, sampler, 0.2126f, 0.0722f, 1);
  REQUIRE(c.ok);
  const int rfull = c.pixels.data[mid + 0];

  CHECK(r709 > r601 + 4);
  CHECK(std::abs(r709 - rfull) > 4);

  gfx::destroy_texture(device, target);
  gfx::destroy_sampler(device, sampler);
  gfx::destroy_texture(device, chroma);
  gfx::destroy_texture(device, luma);
  gfx::destroy_shader(device, shader);
}

TEST_CASE("video shader: the chroma channels convert, right way up") {
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");
  gfx::device &device = fixture.device;

  const gfx::shader_handle shader = load_video(device);
  if (!shader.valid())
    SKIP("shaders are not built in this configuration");

  std::vector<u8> y(nx::cast<usize>(TARGET) * TARGET, 16u);
  for (u32 row = 0; row < TARGET / 2; ++row)
    for (u32 col = 0; col < TARGET; ++col)
      y[nx::cast<usize>(row) * TARGET + col] = 235u;
  const gfx::texture_handle luma = make_luma(device, y);
  const gfx::texture_handle chroma = make_chroma(device, 200u, 128u);
  gfx::wait_uploads(device);
  REQUIRE(luma.valid());
  REQUIRE(chroma.valid());

  const gfx::sampler_handle sampler =
      gfx::create_sampler(device, {.name = "video"});
  REQUIRE(sampler.valid());
  const gfx::texture_handle target = make_target(device);
  REQUIRE(target.valid());

  const Rendered r = draw(device, shader, luma, chroma, target, sampler);
  REQUIRE(r.ok);

  const usize bot = texel(TARGET - 1, TARGET / 2);
  CHECK(int(r.pixels.data[bot + 2]) > int(r.pixels.data[bot + 0]) + 20);
  CHECK(r.pixels.data[texel(0, TARGET / 2) + 1] > 200u);
  CHECK(r.pixels.data[bot + 1] < 40u);

  gfx::destroy_texture(device, target);
  gfx::destroy_sampler(device, sampler);
  gfx::destroy_texture(device, chroma);
  gfx::destroy_texture(device, luma);
  gfx::destroy_shader(device, shader);
}
