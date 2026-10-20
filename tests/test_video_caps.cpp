
#include "framework/nxtest.h"

#include "rendering/rhi/device.h"
#include "video/vulkan/vk_video.h"
#include "video/vulkan/vk_video_client.h"

namespace {

namespace rhi = nxe::rhi;

struct TestDevice {
  rhi::NativeDeviceClient *const clients[1] = {
      &nxm::video::video_decode_client()};
  rhi::Device device;
  bool ready = false;

  TestDevice() {
    rhi::DeviceDesc desc{};
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

  const nxm::video::VideoCodec codecs[] = {nxm::video::VideoCodec::VP9,
                                           nxm::video::VideoCodec::AV1};
  for (const nxm::video::VideoCodec codec : codecs) {
    const nxm::video::VideoDecodeCaps caps =
        nxm::video::query_video_decode_caps(fixture.device, codec);
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

  if (fixture.device.native_queue(nxm::video::video_decode_client()).valid())
    CHECK(nxm::video::query_video_decode_caps(fixture.device,
                                              nxm::video::VideoCodec::VP9)
              .supported);
}

TEST_CASE("video decode: a session and its memory come up, and tear down") {
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");
  if (!fixture.device.native_queue(nxm::video::video_decode_client()).valid())
    SKIP("no hardware video-decode queue");

  nxm::video::VideoDecoder decoder;
  REQUIRE(decoder.init(fixture.device, nxm::video::VideoCodec::VP9, 320, 240));
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
