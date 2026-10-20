#if defined(NX_RHI_VULKAN)

#include "video/vulkan/vk_video_client.h"

#include "video/vulkan/vk_video.h"

namespace gfx = nx::gfx;

namespace nxm::video {
namespace {

[[nodiscard]] VkPhysicalDevice adapter_of(const gfx::native_device &device) {
  return static_cast<VkPhysicalDevice>(device.adapter);
}

} // namespace

void VideoDecodeClient::device_extensions(const gfx::native_device &adapter,
                                          gfx::native_names &out) {
  if (find_vp9_decode_family(adapter_of(adapter)) == ~0u)
    return;
  out.emplace_back(VK_KHR_VIDEO_QUEUE_EXTENSION_NAME);
  out.emplace_back(VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME);
  out.emplace_back(VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME);
}

void VideoDecodeClient::device_features(const gfx::native_device &adapter,
                                        void *const features) {
  if (features == nullptr || find_vp9_decode_family(adapter_of(adapter)) == ~0u)
    return;
  m_vp9.pNext = nullptr;
  m_vp9.videoDecodeVP9 = VK_TRUE;
  auto *link = static_cast<VkBaseOutStructure *>(features);
  while (link->pNext != nullptr)
    link = link->pNext;
  link->pNext = reinterpret_cast<VkBaseOutStructure *>(&m_vp9);
}

u32 VideoDecodeClient::queue_family(const gfx::native_device &adapter) {
  return find_vp9_decode_family(adapter_of(adapter));
}

VideoDecodeClient &video_decode_client() noexcept {
  static VideoDecodeClient client;
  return client;
}

} // namespace nxm::video

#endif
