#pragma once

#include "nx/gfx/native.h"
#include "nx/gfx/vulkan.h"

namespace nxm::video {

/// Asks a Vulkan device, before it is made, for hardware VP9 decode: the video
/// extensions, the VP9 feature and a queue in a decode family of its own. A
/// device made without it, or that could not grant it, has no video queue.
class VideoDecodeClient final : public nx::gfx::native_device_client {
public:
  void device_extensions(const nx::gfx::native_device &adapter,
                         nx::gfx::native_names &out) override;
  void device_features(const nx::gfx::native_device &adapter,
                       void *features) override;
  u32 queue_family(const nx::gfx::native_device &adapter) override;

private:
  /// Chained into the device's features; lives as long as the client.
  VkPhysicalDeviceVideoDecodeVP9FeaturesKHR m_vp9{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_DECODE_VP9_FEATURES_KHR};
};

/// The client a device that should decode video is made with; the video
/// module offers it, and Device::native_queue answers for it.
[[nodiscard]] VideoDecodeClient &video_decode_client() noexcept;

} // namespace nxm::video
