
#include "framework/nxtest.h"

#include "nx/gfx/gfx.h"
#include "nx/gfx/video/client.h"
#include "nx/gfx/video/decoder.h"
#include "owned_device.h"

namespace gfx = nx::gfx;

namespace {

struct TestDevice {
  gfx::native_device_client *const clients[1] = {
      &nx::gfx::video::video_decode_client()};
  nx::gfx::test::OwnedDevice device;
  bool ready = false;

  TestDevice() {
    gfx::device_desc desc{};
    desc.application_name = "nx video caps tests";
    desc.native_clients = clients;
    ready = device.init(desc);
  }
  ~TestDevice() {
    if (ready)
      device.shutdown();
  }
  TestDevice(const TestDevice &) = delete;
  TestDevice &operator=(const TestDevice &) = delete;
};

}

TEST_CASE("video caps: a decode capability query is coherent and never faults") {
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");

  const nx::gfx::video::video_codec codecs[] = {
      nx::gfx::video::video_codec::VP9, nx::gfx::video::video_codec::AV1};
  for (const nx::gfx::video::video_codec codec : codecs) {
    const nx::gfx::video::decode_caps caps =
        nx::gfx::video::query_decode_caps(fixture.device, codec);
    if (caps.supported) {
      CHECK(caps.max_width >= 1920u);
      CHECK(caps.max_height >= 1080u);
      CHECK(caps.max_dpb_slots > 0u);
    } else {
      CHECK(caps.max_width == 0u);
      CHECK(caps.max_height == 0u);
      CHECK(caps.max_dpb_slots == 0u);
    }
  }

  if (gfx::native(fixture.device, nx::gfx::video::video_decode_client())
          .valid())
    CHECK(nx::gfx::video::query_decode_caps(fixture.device,
                                            nx::gfx::video::video_codec::VP9)
              .supported);
}

TEST_CASE("video decode: a session and its memory come up, and tear down") {
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");
  if (!gfx::native(fixture.device, nx::gfx::video::video_decode_client())
           .valid())
    SKIP("no hardware video-decode queue");

  nx::gfx::video::video_decoder decoder;
  REQUIRE(
      decoder.init(fixture.device, nx::gfx::video::video_codec::VP9, 320, 240));
  CHECK(decoder.ready());
  CHECK(decoder.extent().width >= 320u);
  CHECK(decoder.extent().height >= 240u);
  CHECK(decoder.dpb_slots() > 0u);

  CHECK(decoder.dpb_image_count() == decoder.dpb_slots());
  for (u32 slot = 0; slot < decoder.dpb_slots(); ++slot)
    CHECK(decoder.dpb_view(slot) != VK_NULL_HANDLE);

  decoder.shutdown();
  CHECK_FALSE(decoder.ready());
}
