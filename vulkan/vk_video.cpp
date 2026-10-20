#if defined(NX_RHI_VULKAN)

#include "video/vulkan/vk_video.h"
#include "nx/gfx/gfx.h"
#include "nx/gfx/vulkan.h"
#include "video/vulkan/vk_video_client.h"
#include "video/vulkan/vk_vp9.h"

#include "core/foundation/diagnostics/log.h"

#include <cstring>
#include <limits>

namespace gfx = nx::gfx;

namespace nxm::video {

namespace {

[[nodiscard]] bool extension_offered(const VkPhysicalDevice phys,
                                     const char *const name) {
  u32 count = 0;
  if (vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, nullptr) !=
      VK_SUCCESS)
    return false;
  nx::vector<VkExtensionProperties> extensions(count);
  if (vkEnumerateDeviceExtensionProperties(phys, nullptr, &count,
                                           extensions.data()) != VK_SUCCESS)
    return false;
  for (const VkExtensionProperties &e : extensions)
    if (std::strcmp(e.extensionName, name) == 0)
      return true;
  return false;
}

} // namespace

u32 find_vp9_decode_family(const VkPhysicalDevice adapter) {
  constexpr u32 NONE = ~0u;
  if (adapter == VK_NULL_HANDLE)
    return NONE;

  if (!extension_offered(adapter, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME) ||
      !extension_offered(adapter, VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME) ||
      !extension_offered(adapter, VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME))
    return NONE;

  VkPhysicalDeviceVideoDecodeVP9FeaturesKHR vp9{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_DECODE_VP9_FEATURES_KHR};
  VkPhysicalDeviceFeatures2 features{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  features.pNext = &vp9;
  vkGetPhysicalDeviceFeatures2(adapter, &features);
  if (vp9.videoDecodeVP9 != VK_TRUE)
    return NONE;

  u32 count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties2(adapter, &count, nullptr);
  nx::small_vector<VkQueueFamilyProperties2, 8> props(count);
  nx::small_vector<VkQueueFamilyVideoPropertiesKHR, 8> video(count);
  for (u32 i = 0; i < count; ++i) {
    props[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2;
    video[i].sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR;
    props[i].pNext = &video[i];
  }
  vkGetPhysicalDeviceQueueFamilyProperties2(adapter, &count, props.data());
  for (u32 i = 0; i < count; ++i) {
    const VkQueueFlags flags = props[i].queueFamilyProperties.queueFlags;
    if ((flags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) &&
        (video[i].videoCodecOperations &
         VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR) &&
        !(flags & VK_QUEUE_GRAPHICS_BIT))
      return i;
  }
  return NONE;
}

VideoDecodeCaps query_video_decode_caps(const gfx::device &device,
                                        const VideoCodec codec) {
  VideoDecodeCaps out;
  const VkPhysicalDevice phys = gfx::vk::physical_device(device);
  if (phys == VK_NULL_HANDLE ||
      vkGetPhysicalDeviceVideoCapabilitiesKHR == nullptr)
    return out;
  if (!extension_offered(phys, VK_KHR_VIDEO_QUEUE_EXTENSION_NAME))
    return out;

  const char *const codec_extension =
      codec == VideoCodec::AV1 ? VK_KHR_VIDEO_DECODE_AV1_EXTENSION_NAME
                               : VK_KHR_VIDEO_DECODE_VP9_EXTENSION_NAME;
  if (!extension_offered(phys, codec_extension))
    return out;

  VkVideoProfileInfoKHR profile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
  profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
  profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
  profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

  VkVideoDecodeCapabilitiesKHR decode{
      VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR};
  VkVideoCapabilitiesKHR caps{VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR};
  caps.pNext = &decode;

  VkVideoDecodeVP9ProfileInfoKHR vp9{
      VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PROFILE_INFO_KHR};
  VkVideoDecodeVP9CapabilitiesKHR vp9_caps{
      VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_CAPABILITIES_KHR};
  VkVideoDecodeAV1ProfileInfoKHR av1{
      VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR};
  VkVideoDecodeAV1CapabilitiesKHR av1_caps{
      VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_KHR};

  if (codec == VideoCodec::AV1) {
    av1.stdProfile = STD_VIDEO_AV1_PROFILE_MAIN;
    av1.filmGrainSupport = VK_FALSE;
    profile.pNext = &av1;
    profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR;
    decode.pNext = &av1_caps;
  } else {
    vp9.stdProfile = STD_VIDEO_VP9_PROFILE_0;
    profile.pNext = &vp9;
    profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR;
    decode.pNext = &vp9_caps;
  }

  if (vkGetPhysicalDeviceVideoCapabilitiesKHR(phys, &profile, &caps) !=
      VK_SUCCESS)
    return out;

  out.supported = true;
  out.max_width = caps.maxCodedExtent.width;
  out.max_height = caps.maxCodedExtent.height;
  out.max_dpb_slots = caps.maxDpbSlots;
  out.dpb_and_output_coincide =
      (decode.flags &
       VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR) != 0;
  return out;
}

namespace {

[[nodiscard]] bool align_up(const u32 value, const u32 to, u32 &out) noexcept {
  const u64 alignment = nx::max<u64>(to, 1);
  const u64 aligned =
      (nx::cast<u64>(value) + alignment - 1) / alignment * alignment;
  if (aligned > std::numeric_limits<u32>::max())
    return false;
  out = nx::cast<u32>(aligned);
  return true;
}

[[nodiscard]] VkFormat first_video_format(const VkPhysicalDevice phys,
                                          const VkVideoProfileListInfoKHR &list,
                                          const VkImageUsageFlags usage) {
  VkPhysicalDeviceVideoFormatInfoKHR info{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR};
  info.pNext = &list;
  info.imageUsage = usage;

  u32 count = 0;
  if (vkGetPhysicalDeviceVideoFormatPropertiesKHR(phys, &info, &count,
                                                  nullptr) != VK_SUCCESS ||
      count == 0)
    return VK_FORMAT_UNDEFINED;
  nx::small_vector<VkVideoFormatPropertiesKHR, 8> formats(count);
  for (VkVideoFormatPropertiesKHR &f : formats)
    f.sType = VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR;
  if (vkGetPhysicalDeviceVideoFormatPropertiesKHR(phys, &info, &count,
                                                  formats.data()) != VK_SUCCESS)
    return VK_FORMAT_UNDEFINED;
  return formats[0].format;
}

[[nodiscard]] u32 session_memory_type(const VkPhysicalDevice phys,
                                      const u32 type_bits) {
  VkPhysicalDeviceMemoryProperties props;
  vkGetPhysicalDeviceMemoryProperties(phys, &props);
  for (u32 i = 0; i < props.memoryTypeCount; ++i)
    if ((type_bits & (1u << i)) && (props.memoryTypes[i].propertyFlags &
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
      return i;
  for (u32 i = 0; i < props.memoryTypeCount; ++i)
    if (type_bits & (1u << i))
      return i;
  return ~0u;
}

[[nodiscard]] bool
create_video_image(const gfx::device &owner, const VkPhysicalDevice phys,
                   const VkVideoProfileListInfoKHR &profiles,
                   const VkFormat format, const VkExtent2D extent,
                   const VkImageUsageFlags usage, const u32 *const families,
                   const u32 family_count, VideoImage &out) {
  const VkDevice dev = gfx::vk::logical_device(owner);
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.pNext = &profiles;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = format;
  ci.extent = {extent.width, extent.height, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = usage;
  // The decode queue writes these; a transfer or graphics queue reads them
  // back. Concurrent sharing spares an ownership hand-off between the two.
  if (family_count > 1) {
    ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
    ci.queueFamilyIndexCount = family_count;
    ci.pQueueFamilyIndices = families;
  } else {
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (NX_GFX_VK_CHECK(owner, vkCreateImage(dev, &ci, nullptr, &out.image)) !=
      VK_SUCCESS)
    return false;

  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(dev, out.image, &mr);
  const u32 type = session_memory_type(phys, mr.memoryTypeBits);
  if (type == ~0u)
    return false;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = type;
  if (NX_GFX_VK_CHECK(owner, vkAllocateMemory(dev, &ai, nullptr,
                                              &out.memory)) != VK_SUCCESS)
    return false;
  if (NX_GFX_VK_CHECK(owner, vkBindImageMemory(dev, out.image, out.memory,
                                               0)) != VK_SUCCESS)
    return false;

  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = out.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return NX_GFX_VK_CHECK(owner, vkCreateImageView(dev, &vi, nullptr,
                                                  &out.view)) == VK_SUCCESS;
}

void destroy_video_image(const VkDevice dev, VideoImage &image) {
  if (image.view != VK_NULL_HANDLE)
    vkDestroyImageView(dev, image.view, nullptr);
  if (image.image != VK_NULL_HANDLE)
    vkDestroyImage(dev, image.image, nullptr);
  if (image.memory != VK_NULL_HANDLE)
    vkFreeMemory(dev, image.memory, nullptr);
  image = {};
}

[[nodiscard]] u64 align_up64(const u64 value, const u64 to) {
  return to <= 1 ? value : (value + to - 1) / to * to;
}

[[nodiscard]] u32 host_memory_type(const VkPhysicalDevice phys,
                                   const u32 type_bits) {
  VkPhysicalDeviceMemoryProperties props;
  vkGetPhysicalDeviceMemoryProperties(phys, &props);
  const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for (u32 i = 0; i < props.memoryTypeCount; ++i)
    if ((type_bits & (1u << i)) &&
        (props.memoryTypes[i].propertyFlags & want) == want)
      return i;
  return ~0u;
}

[[nodiscard]] bool create_host_buffer(const gfx::device &owner,
                                      const VkPhysicalDevice phys,
                                      const u64 size,
                                      const VkBufferUsageFlags usage,
                                      const void *const pNext, VkBuffer &buffer,
                                      VkDeviceMemory &memory, void *&mapped) {
  const VkDevice dev = gfx::vk::logical_device(owner);
  VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ci.pNext = pNext;
  ci.size = size;
  ci.usage = usage;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (NX_GFX_VK_CHECK(owner, vkCreateBuffer(dev, &ci, nullptr, &buffer)) !=
      VK_SUCCESS)
    return false;

  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(dev, buffer, &mr);
  const u32 type = host_memory_type(phys, mr.memoryTypeBits);
  if (type == ~0u)
    return false;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = type;
  if (NX_GFX_VK_CHECK(owner, vkAllocateMemory(dev, &ai, nullptr, &memory)) !=
      VK_SUCCESS)
    return false;
  if (NX_GFX_VK_CHECK(owner, vkBindBufferMemory(dev, buffer, memory, 0)) !=
      VK_SUCCESS)
    return false;
  return NX_GFX_VK_CHECK(owner, vkMapMemory(dev, memory, 0, VK_WHOLE_SIZE, 0,
                                            &mapped)) == VK_SUCCESS;
}

} // namespace

VideoDecoder::~VideoDecoder() { shutdown(); }

void VideoDecoder::shutdown() noexcept {
  if (m_device != nullptr) {
    const VkDevice dev = gfx::vk::logical_device(*m_device);
    if (gfx::device_status(*m_device) == gfx::gpu_device_status::Ready)
      NX_GFX_VK_CHECK(*m_device, vkDeviceWaitIdle(dev));

    if (m_y.valid())
      gfx::destroy_texture(*m_device, m_y);
    if (m_cbcr.valid())
      gfx::destroy_texture(*m_device, m_cbcr);
    destroy_video_image(dev, m_sampleable);
    if (m_gfx_fence != VK_NULL_HANDLE)
      vkDestroyFence(dev, m_gfx_fence, nullptr);
    if (m_gfx_pool != VK_NULL_HANDLE)
      vkDestroyCommandPool(dev, m_gfx_pool, nullptr);

    if (m_bitstream_mapped != nullptr)
      vkUnmapMemory(dev, m_bitstream_memory);
    if (m_bitstream != VK_NULL_HANDLE)
      vkDestroyBuffer(dev, m_bitstream, nullptr);
    if (m_bitstream_memory != VK_NULL_HANDLE)
      vkFreeMemory(dev, m_bitstream_memory, nullptr);
    if (m_fence != VK_NULL_HANDLE)
      vkDestroyFence(dev, m_fence, nullptr);
    if (m_pool != VK_NULL_HANDLE)
      vkDestroyCommandPool(dev, m_pool, nullptr);
    for (VideoImage &image : m_dpb)
      destroy_video_image(dev, image);
    destroy_video_image(dev, m_output);
    if (m_session != VK_NULL_HANDLE)
      vkDestroyVideoSessionKHR(dev, m_session, nullptr);
    for (const VkDeviceMemory memory : m_memories)
      vkFreeMemory(dev, memory, nullptr);
  }
  m_session = VK_NULL_HANDLE;
  m_memories.clear();
  m_dpb.clear();
  m_output = {};
  m_sampleable = {};
  m_coincide = false;
  m_pool = VK_NULL_HANDLE;
  m_cmd = VK_NULL_HANDLE;
  m_fence = VK_NULL_HANDLE;
  m_video_supports_transfer = false;
  m_needs_reset = true;
  m_bitstream = VK_NULL_HANDLE;
  m_bitstream_memory = VK_NULL_HANDLE;
  m_bitstream_mapped = nullptr;
  m_bitstream_capacity = 0;
  m_bitstream_offset_align = 1;
  m_bitstream_size_align = 1;
  for (u32 i = 0; i < 8; ++i)
    m_vp9_valid[i] = false;
  m_next_slot = 0;
  m_last_slot = 0;
  m_have_last = false;
  m_gfx_pool = VK_NULL_HANDLE;
  m_gfx_cmd = VK_NULL_HANDLE;
  m_gfx_fence = VK_NULL_HANDLE;
  m_y = {};
  m_cbcr = {};
  m_device = nullptr;
  m_picture_format = VK_FORMAT_UNDEFINED;
  m_dpb_format = VK_FORMAT_UNDEFINED;
  m_extent = {};
  m_coded_limit = {};
  m_max_dpb_slots = 0;
}

bool VideoDecoder::init(gfx::device &device, const VideoCodec codec,
                        const u32 width, const u32 height) {
  shutdown();
  const gfx::native_queue video = gfx::native(device, video_decode_client());
  if (!video.valid() || codec != VideoCodec::VP9)
    return false;

  m_device = &device;
  m_video_queue = static_cast<VkQueue>(video.queue);
  m_video_family = video.family;
  const VkPhysicalDevice phys = gfx::vk::physical_device(device);
  const VkDevice dev = gfx::vk::logical_device(device);

  VkVideoDecodeVP9ProfileInfoKHR vp9{
      VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PROFILE_INFO_KHR};
  vp9.stdProfile = STD_VIDEO_VP9_PROFILE_0;
  VkVideoProfileInfoKHR profile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
  profile.pNext = &vp9;
  profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR;
  profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
  profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
  profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

  VkVideoDecodeVP9CapabilitiesKHR vp9_caps{
      VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_CAPABILITIES_KHR};
  VkVideoDecodeCapabilitiesKHR decode_caps{
      VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR};
  decode_caps.pNext = &vp9_caps;
  VkVideoCapabilitiesKHR caps{VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR};
  caps.pNext = &decode_caps;
  if (const VkResult r =
          vkGetPhysicalDeviceVideoCapabilitiesKHR(phys, &profile, &caps);
      r != VK_SUCCESS) {
    nx::logw(nx::log::rhi, "video: capabilities query failed ({})",
             nx::cast<i32>(r));
    m_device = nullptr;
    return false;
  }

  constexpr u32 MIN_VP9_DPB_SLOTS = 9;
  constexpr u32 MAX_TRACKED_DPB_SLOTS = 16;
  if (width == 0 || height == 0 || caps.maxCodedExtent.width == 0 ||
      caps.maxCodedExtent.height == 0 ||
      caps.minCodedExtent.width > caps.maxCodedExtent.width ||
      caps.minCodedExtent.height > caps.maxCodedExtent.height ||
      caps.maxDpbSlots < MIN_VP9_DPB_SLOTS ||
      caps.maxActiveReferencePictures < 3) {
    nx::logw(nx::log::rhi, "video: unusable decode capabilities");
    m_device = nullptr;
    return false;
  }

  u32 aligned_width = 0;
  u32 aligned_height = 0;
  if (!align_up(width, caps.pictureAccessGranularity.width, aligned_width) ||
      !align_up(height, caps.pictureAccessGranularity.height, aligned_height) ||
      aligned_width > caps.maxCodedExtent.width ||
      aligned_height > caps.maxCodedExtent.height) {
    nx::logw(nx::log::rhi, "video: requested decode extent {}x{} is unsupported",
             width, height);
    m_device = nullptr;
    return false;
  }

  VkVideoProfileListInfoKHR profiles{
      VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
  profiles.profileCount = 1;
  profiles.pProfiles = &profile;
  m_coincide =
      (decode_caps.flags &
       VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR) != 0;
  const bool coincide = m_coincide;
  if (coincide) {
    m_dpb_format =
        first_video_format(phys, profiles,
                           VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR |
                               VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR);
    m_picture_format = m_dpb_format;
  } else {
    m_dpb_format = first_video_format(phys, profiles,
                                      VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR);
    m_picture_format = first_video_format(
        phys, profiles, VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR);
  }
  if (m_dpb_format == VK_FORMAT_UNDEFINED ||
      m_picture_format == VK_FORMAT_UNDEFINED) {
    nx::logw(nx::log::rhi, "video: no decode image format for the profile");
    m_device = nullptr;
    return false;
  }

  m_extent.width = nx::max(aligned_width, caps.minCodedExtent.width);
  m_extent.height = nx::max(aligned_height, caps.minCodedExtent.height);
  m_coded_limit = {width, height};
  m_max_dpb_slots = nx::min(caps.maxDpbSlots, MAX_TRACKED_DPB_SLOTS);
  m_bitstream_offset_align =
      nx::max<u64>(1, caps.minBitstreamBufferOffsetAlignment);
  m_bitstream_size_align =
      nx::max<u64>(1, caps.minBitstreamBufferSizeAlignment);

  VkVideoSessionCreateInfoKHR create{
      VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR};
  create.queueFamilyIndex = m_video_family;
  create.pVideoProfile = &profile;
  create.pictureFormat = m_picture_format;
  create.maxCodedExtent = m_extent;
  create.referencePictureFormat = m_dpb_format;
  create.maxDpbSlots = m_max_dpb_slots;
  create.maxActiveReferencePictures = 3;
  create.pStdHeaderVersion = &caps.stdHeaderVersion;
  if (const VkResult r = NX_GFX_VK_CHECK(
          device, vkCreateVideoSessionKHR(dev, &create, nullptr, &m_session));
      r != VK_SUCCESS) {
    nx::logw(nx::log::rhi, "video: vkCreateVideoSessionKHR failed ({})",
             nx::cast<i32>(r));
    m_session = VK_NULL_HANDLE;
    m_device = nullptr;
    return false;
  }

  u32 requirement_count = 0;
  if (NX_GFX_VK_CHECK(device, vkGetVideoSessionMemoryRequirementsKHR(
                                  dev, m_session, &requirement_count,
                                  nullptr)) != VK_SUCCESS) {
    shutdown();
    return false;
  }
  nx::small_vector<VkVideoSessionMemoryRequirementsKHR, 4> requirements(
      requirement_count);
  for (VkVideoSessionMemoryRequirementsKHR &r : requirements)
    r.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR;
  if (NX_GFX_VK_CHECK(device, vkGetVideoSessionMemoryRequirementsKHR(
                                  dev, m_session, &requirement_count,
                                  requirements.data())) != VK_SUCCESS) {
    shutdown();
    return false;
  }

  nx::small_vector<VkBindVideoSessionMemoryInfoKHR, 4> binds(requirement_count);
  for (u32 i = 0; i < requirement_count; ++i) {
    const VkMemoryRequirements &mr = requirements[i].memoryRequirements;
    const u32 type = session_memory_type(phys, mr.memoryTypeBits);
    if (type == ~0u) {
      nx::logw(nx::log::rhi, "video: no memory type for the session");
      shutdown();
      return false;
    }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = mr.size;
    allocate.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (const VkResult r = NX_GFX_VK_CHECK(
            device, vkAllocateMemory(dev, &allocate, nullptr, &memory));
        r != VK_SUCCESS) {
      nx::logw(nx::log::rhi, "video: session memory alloc failed ({})",
               nx::cast<i32>(r));
      shutdown();
      return false;
    }
    m_memories.push_back(memory);

    binds[i].sType = VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR;
    binds[i].memoryBindIndex = requirements[i].memoryBindIndex;
    binds[i].memory = memory;
    binds[i].memoryOffset = 0;
    binds[i].memorySize = mr.size;
  }
  if (const VkResult r = NX_GFX_VK_CHECK(
          device, vkBindVideoSessionMemoryKHR(dev, m_session, requirement_count,
                                              binds.data()));
      r != VK_SUCCESS) {
    nx::logw(nx::log::rhi, "video: bind session memory failed ({})",
             nx::cast<i32>(r));
    shutdown();
    return false;
  }

  const u32 families[2] = {m_video_family, gfx::vk::graphics_family(device)};
  const VkImageUsageFlags dpb_usage =
      (coincide ? (VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR |
                   VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR)
                : VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR) |
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  m_dpb.resize(m_max_dpb_slots);
  for (VideoImage &slot : m_dpb) {
    if (!create_video_image(device, phys, profiles, m_dpb_format, m_extent,
                            dpb_usage, families, 2, slot)) {
      nx::logw(nx::log::rhi, "video: dpb image allocation failed");
      shutdown();
      return false;
    }
  }
  if (!coincide &&
      !create_video_image(device, phys, profiles, m_picture_format, m_extent,
                          VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          families, 2, m_output)) {
    nx::logw(nx::log::rhi, "video: output image allocation failed");
    shutdown();
    return false;
  }

  if (!create_command_resources() ||
      !create_bitstream_buffer(size_t{1} << 20, profiles) ||
      !create_sampleable_image(families, 2) || !create_plane_resources()) {
    shutdown();
    return false;
  }

  nx::logd(nx::log::rhi, "video: decode session {}x{}, {} dpb slots",
           m_extent.width, m_extent.height, m_max_dpb_slots);
  return true;
}

bool VideoDecoder::create_command_resources() {
  const VkDevice dev = gfx::vk::logical_device(*m_device);
  const u32 video_family = m_video_family;

  u32 count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(gfx::vk::physical_device(*m_device),
                                           &count, nullptr);
  nx::small_vector<VkQueueFamilyProperties, 8> props(count);
  vkGetPhysicalDeviceQueueFamilyProperties(gfx::vk::physical_device(*m_device),
                                           &count, props.data());
  m_video_supports_transfer =
      video_family < count &&
      (props[video_family].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0;

  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool.queueFamilyIndex = video_family;
  if (NX_GFX_VK_CHECK(*m_device, vkCreateCommandPool(dev, &pool, nullptr,
                                                     &m_pool)) != VK_SUCCESS)
    return false;

  VkCommandBufferAllocateInfo alloc{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = m_pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  if (NX_GFX_VK_CHECK(*m_device, vkAllocateCommandBuffers(
                                     dev, &alloc, &m_cmd)) != VK_SUCCESS)
    return false;

  VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  return NX_GFX_VK_CHECK(*m_device, vkCreateFence(dev, &fence, nullptr,
                                                  &m_fence)) == VK_SUCCESS;
}

bool VideoDecoder::create_bitstream_buffer(
    const u64 capacity, const VkVideoProfileListInfoKHR &profiles) {
  m_bitstream_capacity = align_up64(capacity, m_bitstream_size_align);
  return create_host_buffer(
      *m_device, gfx::vk::physical_device(*m_device), m_bitstream_capacity,
      VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR, &profiles, m_bitstream,
      m_bitstream_memory, m_bitstream_mapped);
}

u32 VideoDecoder::allocate_slot() noexcept {
  for (u32 tries = 0; tries < m_max_dpb_slots; ++tries) {
    const u32 slot = m_next_slot;
    m_next_slot = (m_next_slot + 1) % m_max_dpb_slots;
    bool referenced = false;
    for (u32 i = 0; i < 8; ++i)
      if (m_vp9_valid[i] && m_vp9_to_dpb[i] == slot) {
        referenced = true;
        break;
      }
    if (!referenced)
      return slot;
  }
  return m_next_slot;
}

bool VideoDecoder::decode(const u8 *const frame, const u32 size,
                          DecodedPicture &out) {
  out = DecodedPicture{};
  if (!ready() || frame == nullptr || size == 0)
    return false;

  u32 ref_w[8];
  u32 ref_h[8];
  for (u32 i = 0; i < 8; ++i) {
    const bool live = m_vp9_valid[i];
    ref_w[i] = live ? m_slot_width[m_vp9_to_dpb[i]] : 0;
    ref_h[i] = live ? m_slot_height[m_vp9_to_dpb[i]] : 0;
  }

  Vp9FrameHeader hdr;
  if (!parse_vp9_frame_header(frame, size, ref_w, ref_h, hdr))
    return false;

  if (hdr.show_existing_frame) {
    const u32 buf = hdr.frame_to_show_map_idx;
    if (buf >= 8 || !m_vp9_valid[buf])
      return false;
    out.ok = true;
    out.show = true;
    out.slot = m_vp9_to_dpb[buf];
    out.width = m_slot_width[out.slot];
    out.height = m_slot_height[out.slot];
    return true;
  }

  const bool carries_colour =
      hdr.picture.frame_type == STD_VIDEO_VP9_FRAME_TYPE_KEY ||
      hdr.picture.flags.intra_only != 0;
  if (hdr.picture.profile != STD_VIDEO_VP9_PROFILE_0 ||
      (carries_colour &&
       (hdr.color.BitDepth != 8 || hdr.color.subsampling_x != 1 ||
        hdr.color.subsampling_y != 1)))
    return false;

  const u32 coded_w = hdr.frame_width != 0 ? hdr.frame_width : m_extent.width;
  const u32 coded_h =
      hdr.frame_height != 0 ? hdr.frame_height : m_extent.height;
  if (coded_w == 0 || coded_h == 0 || coded_w > m_coded_limit.width ||
      coded_h > m_coded_limit.height)
    return false;

  const u64 range = align_up64(size, m_bitstream_size_align);
  if (range > m_bitstream_capacity)
    return false;
  std::memcpy(m_bitstream_mapped, frame, size);
  if (range > size)
    std::memset(static_cast<u8 *>(m_bitstream_mapped) + size, 0,
                nx::cast<usize>(range - size));

  const u32 setup = allocate_slot();

  hdr.picture.pColorConfig = &hdr.color;
  hdr.picture.pLoopFilter = &hdr.loop_filter;
  hdr.picture.pSegmentation = &hdr.segmentation;

  VkVideoDecodeVP9PictureInfoKHR vp9pic{
      VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PICTURE_INFO_KHR};
  vp9pic.pStdPictureInfo = &hdr.picture;
  for (u32 k = 0; k < VK_MAX_VIDEO_VP9_REFERENCES_PER_FRAME_KHR; ++k)
    vp9pic.referenceNameSlotIndices[k] = -1;
  const bool inter =
      hdr.picture.frame_type == STD_VIDEO_VP9_FRAME_TYPE_NON_KEY &&
      hdr.picture.flags.intra_only == 0;
  if (inter)
    for (u32 k = 0; k < VK_MAX_VIDEO_VP9_REFERENCES_PER_FRAME_KHR; ++k) {
      const u32 buf = hdr.ref_frame_idx[k];
      if (buf >= 8 || !m_vp9_valid[buf])
        return false;
      vp9pic.referenceNameSlotIndices[k] = nx::cast<i32>(m_vp9_to_dpb[buf]);
    }
  vp9pic.uncompressedHeaderOffset = 0;
  vp9pic.compressedHeaderOffset = hdr.uncompressed_header_size;
  vp9pic.tilesOffset =
      hdr.uncompressed_header_size + hdr.compressed_header_size;

  VkVideoPictureResourceInfoKHR setup_res{
      VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
  setup_res.codedOffset = {0, 0};
  setup_res.codedExtent = {coded_w, coded_h};
  setup_res.baseArrayLayer = 0;
  setup_res.imageViewBinding = m_dpb[setup].view;

  VkVideoPictureResourceInfoKHR ref_res[8];
  VkVideoReferenceSlotInfoKHR decode_refs[8];
  u32 unique[8];
  u32 ref_count = 0;
  for (u32 k = 0; k < VK_MAX_VIDEO_VP9_REFERENCES_PER_FRAME_KHR; ++k) {
    const i32 s = vp9pic.referenceNameSlotIndices[k];
    if (s < 0)
      continue;
    bool seen = false;
    for (u32 j = 0; j < ref_count; ++j)
      if (unique[j] == nx::cast<u32>(s))
        seen = true;
    if (seen)
      continue;
    unique[ref_count] = nx::cast<u32>(s);
    ref_res[ref_count] = VkVideoPictureResourceInfoKHR{
        VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    ref_res[ref_count].codedOffset = {0, 0};
    ref_res[ref_count].codedExtent = {m_slot_width[nx::cast<u32>(s)],
                                      m_slot_height[nx::cast<u32>(s)]};
    ref_res[ref_count].baseArrayLayer = 0;
    ref_res[ref_count].imageViewBinding = m_dpb[nx::cast<u32>(s)].view;
    decode_refs[ref_count] = VkVideoReferenceSlotInfoKHR{
        VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
    decode_refs[ref_count].slotIndex = s;
    decode_refs[ref_count].pPictureResource = &ref_res[ref_count];
    ++ref_count;
  }

  VkVideoReferenceSlotInfoKHR begin_slots[9];
  for (u32 j = 0; j < ref_count; ++j)
    begin_slots[j] = decode_refs[j];
  begin_slots[ref_count] = VkVideoReferenceSlotInfoKHR{
      VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
  begin_slots[ref_count].slotIndex = -1;
  begin_slots[ref_count].pPictureResource = &setup_res;

  VkVideoReferenceSlotInfoKHR setup_slot{
      VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
  setup_slot.slotIndex = nx::cast<i32>(setup);
  setup_slot.pPictureResource = &setup_res;

  VkVideoPictureResourceInfoKHR dst_res = setup_res;
  if (!m_coincide)
    dst_res.imageViewBinding = m_output.view;

  VkVideoDecodeInfoKHR di{VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR};
  di.pNext = &vp9pic;
  di.srcBuffer = m_bitstream;
  di.srcBufferOffset = 0;
  di.srcBufferRange = range;
  di.dstPictureResource = dst_res;
  di.pSetupReferenceSlot = &setup_slot;
  di.referenceSlotCount = ref_count;
  di.pReferenceSlots = ref_count != 0 ? decode_refs : nullptr;

  const VkDevice dev = gfx::vk::logical_device(*m_device);
  NX_GFX_VK_TRY(*m_device, vkResetCommandBuffer(m_cmd, 0));
  VkCommandBufferBeginInfo begin_cmd{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_cmd.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  NX_GFX_VK_TRY(*m_device, vkBeginCommandBuffer(m_cmd, &begin_cmd));

  // Bring every image the decode touches into its decode layout. References are
  // already there from earlier decodes and skip the barrier.
  VkImageMemoryBarrier2 barriers[10];
  u32 barrier_count = 0;
  const auto to_layout = [&](VideoImage &img, const VkImageLayout layout) {
    if (img.layout == layout)
      return;
    VkImageMemoryBarrier2 &b = barriers[barrier_count++];
    b = VkImageMemoryBarrier2{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
    b.srcAccessMask = 0;
    b.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR;
    b.dstAccessMask = VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR |
                      VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR;
    b.oldLayout = img.layout;
    b.newLayout = layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    img.layout = layout;
  };
  to_layout(m_dpb[setup], VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR);
  if (!m_coincide)
    to_layout(m_output, VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR);
  for (u32 j = 0; j < ref_count; ++j)
    to_layout(m_dpb[unique[j]], VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR);
  if (barrier_count != 0) {
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = barrier_count;
    dep.pImageMemoryBarriers = barriers;
    vkCmdPipelineBarrier2KHR(m_cmd, &dep);
  }

  VkVideoBeginCodingInfoKHR begin{
      VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
  begin.videoSession = m_session;
  begin.referenceSlotCount = ref_count + 1;
  begin.pReferenceSlots = begin_slots;
  vkCmdBeginVideoCodingKHR(m_cmd, &begin);

  if (m_needs_reset) {
    VkVideoCodingControlInfoKHR control{
        VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR};
    control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
    vkCmdControlVideoCodingKHR(m_cmd, &control);
    m_needs_reset = false;
  }

  vkCmdDecodeVideoKHR(m_cmd, &di);

  VkVideoEndCodingInfoKHR end{VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
  vkCmdEndVideoCodingKHR(m_cmd, &end);
  NX_GFX_VK_TRY(*m_device, vkEndCommandBuffer(m_cmd));

  NX_GFX_VK_TRY(*m_device, vkResetFences(dev, 1, &m_fence));
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &m_cmd;
  NX_GFX_VK_TRY(*m_device, vkQueueSubmit(m_video_queue, 1, &submit, m_fence));
  NX_GFX_VK_TRY(*m_device, vkWaitForFences(dev, 1, &m_fence, VK_TRUE, ~0ull));

  m_slot_width[setup] = coded_w;
  m_slot_height[setup] = coded_h;
  for (u32 i = 0; i < 8; ++i)
    if (hdr.picture.refresh_frame_flags & (1u << i)) {
      m_vp9_to_dpb[i] = setup;
      m_vp9_valid[i] = true;
    }

  out.ok = true;
  out.slot = setup;
  out.show = hdr.picture.flags.show_frame != 0;
  out.width = coded_w;
  out.height = coded_h;
  return true;
}

bool VideoDecoder::create_sampleable_image(const u32 *const families,
                                           const u32 family_count) {
  const VkDevice dev = gfx::vk::logical_device(*m_device);
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = m_picture_format;
  ci.extent = {m_extent.width, m_extent.height, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
             VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (family_count > 1) {
    ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
    ci.queueFamilyIndexCount = family_count;
    ci.pQueueFamilyIndices = families;
  } else {
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  }
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (NX_GFX_VK_CHECK(*m_device,
                      vkCreateImage(dev, &ci, nullptr, &m_sampleable.image)) !=
      VK_SUCCESS)
    return false;

  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(dev, m_sampleable.image, &mr);
  const u32 type = session_memory_type(gfx::vk::physical_device(*m_device),
                                       mr.memoryTypeBits);
  if (type == ~0u)
    return false;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = type;
  if (NX_GFX_VK_CHECK(*m_device, vkAllocateMemory(dev, &ai, nullptr,
                                                  &m_sampleable.memory)) !=
      VK_SUCCESS)
    return false;
  return NX_GFX_VK_CHECK(*m_device, vkBindImageMemory(dev, m_sampleable.image,
                                                      m_sampleable.memory,
                                                      0)) == VK_SUCCESS;
}

bool VideoDecoder::present(const u32 slot) {
  if (!ready() || !m_video_supports_transfer)
    return false;
  VideoImage &src = m_coincide ? m_dpb[slot] : m_output;
  if (m_coincide && slot >= m_dpb.size())
    return false;
  const VkImageLayout restore = m_coincide
                                    ? VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR
                                    : VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR;

  const VkDevice dev = gfx::vk::logical_device(*m_device);
  NX_GFX_VK_TRY(*m_device, vkResetCommandBuffer(m_cmd, 0));
  VkCommandBufferBeginInfo begin_cmd{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_cmd.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  NX_GFX_VK_TRY(*m_device, vkBeginCommandBuffer(m_cmd, &begin_cmd));

  const auto barrier = [&](VideoImage &img, const VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask =
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask =
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = img.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    img.layout = to;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2KHR(m_cmd, &dep);
  };

  barrier(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  barrier(m_sampleable, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  VkImageCopy regions[2]{};
  regions[0].srcSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
  regions[0].dstSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
  regions[0].extent = {m_extent.width, m_extent.height, 1};
  regions[1].srcSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
  regions[1].dstSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
  regions[1].extent = {m_extent.width / 2, m_extent.height / 2, 1};
  vkCmdCopyImage(m_cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 m_sampleable.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2,
                 regions);

  barrier(src, restore);
  barrier(m_sampleable, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  NX_GFX_VK_TRY(*m_device, vkEndCommandBuffer(m_cmd));

  NX_GFX_VK_TRY(*m_device, vkResetFences(dev, 1, &m_fence));
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &m_cmd;
  NX_GFX_VK_TRY(*m_device, vkQueueSubmit(m_video_queue, 1, &submit, m_fence));
  NX_GFX_VK_TRY(*m_device, vkWaitForFences(dev, 1, &m_fence, VK_TRUE, ~0ull));
  return true;
}

bool VideoDecoder::create_plane_resources() {
  const VkDevice dev = gfx::vk::logical_device(*m_device);

  VkCommandPoolCreateInfo cpc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpc.queueFamilyIndex = gfx::vk::graphics_family(*m_device);
  if (NX_GFX_VK_CHECK(*m_device,
                      vkCreateCommandPool(dev, &cpc, nullptr, &m_gfx_pool)) !=
      VK_SUCCESS)
    return false;
  VkCommandBufferAllocateInfo cba{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cba.commandPool = m_gfx_pool;
  cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cba.commandBufferCount = 1;
  if (NX_GFX_VK_CHECK(*m_device, vkAllocateCommandBuffers(
                                     dev, &cba, &m_gfx_cmd)) != VK_SUCCESS)
    return false;
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (NX_GFX_VK_CHECK(*m_device, vkCreateFence(dev, &fci, nullptr,
                                               &m_gfx_fence)) != VK_SUCCESS)
    return false;

  gfx::texture_desc luma;
  luma.name = "video luma";
  luma.format = gfx::format::R8_UNORM;
  luma.width = m_extent.width;
  luma.height = m_extent.height;
  luma.usage = gfx::texture_usage::Sampled | gfx::texture_usage::CopyDst |
               gfx::texture_usage::CopySrc;
  m_y = gfx::create_texture(*m_device, luma);

  gfx::texture_desc chroma;
  chroma.name = "video chroma";
  chroma.format = gfx::format::RG8_UNORM;
  chroma.width = m_extent.width / 2;
  chroma.height = m_extent.height / 2;
  chroma.usage = gfx::texture_usage::Sampled | gfx::texture_usage::CopyDst |
                 gfx::texture_usage::CopySrc;
  m_cbcr = gfx::create_texture(*m_device, chroma);

  return m_y.valid() && m_cbcr.valid();
}

bool VideoDecoder::decode_frame(const u8 *const frame, const u32 size,
                                bool &shown, u32 &width, u32 &height) {
  DecodedPicture pic;
  if (!decode(frame, size, pic)) {
    m_have_last = false;
    return false;
  }
  m_last_slot = pic.slot;
  m_have_last = true;
  shown = pic.show;
  width = pic.width;
  height = pic.height;
  return true;
}

void VideoDecoder::reset_stream() noexcept {
  for (u32 i = 0; i < 8; ++i)
    m_vp9_valid[i] = false;
  m_have_last = false;
  m_needs_reset = true;
}

bool VideoDecoder::show_last() {
  if (!m_have_last || !present(m_last_slot))
    return false;

  const VkImage y = static_cast<VkImage>(gfx::native(*m_device, m_y).image);
  const VkImage c = static_cast<VkImage>(gfx::native(*m_device, m_cbcr).image);
  if (y == VK_NULL_HANDLE || c == VK_NULL_HANDLE)
    return false;
  const VkDevice dev = gfx::vk::logical_device(*m_device);

  NX_GFX_VK_TRY(*m_device, vkResetCommandBuffer(m_gfx_cmd, 0));
  VkCommandBufferBeginInfo begin_cmd{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin_cmd.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  NX_GFX_VK_TRY(*m_device, vkBeginCommandBuffer(m_gfx_cmd, &begin_cmd));

  const auto barrier = [&](const VkImage image, const VkImageLayout from,
                           const VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask =
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask =
        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2KHR(m_gfx_cmd, &d);
  };

  barrier(m_sampleable.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  barrier(y, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  barrier(c, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  const auto copy_plane = [&](const VkImageAspectFlagBits src_aspect,
                              const VkImage dst, const u32 w, const u32 h) {
    VkImageCopy region{};
    region.srcSubresource = {nx::cast<VkImageAspectFlags>(src_aspect), 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {w, h, 1};
    vkCmdCopyImage(m_gfx_cmd, m_sampleable.image,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  };
  copy_plane(VK_IMAGE_ASPECT_PLANE_0_BIT, y, m_extent.width, m_extent.height);
  copy_plane(VK_IMAGE_ASPECT_PLANE_1_BIT, c, m_extent.width / 2,
             m_extent.height / 2);

  barrier(y, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  barrier(c, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  barrier(m_sampleable.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  NX_GFX_VK_TRY(*m_device, vkEndCommandBuffer(m_gfx_cmd));

  NX_GFX_VK_TRY(*m_device, vkResetFences(dev, 1, &m_gfx_fence));
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &m_gfx_cmd;
  NX_GFX_VK_TRY(*m_device,
                gfx::vk::submit_graphics(*m_device, {&submit, 1}, m_gfx_fence));
  NX_GFX_VK_TRY(*m_device,
                vkWaitForFences(dev, 1, &m_gfx_fence, VK_TRUE, ~0ull));

  gfx::set_texture_state(*m_device, m_y, gfx::resource_state::ShaderReadOnly);
  gfx::set_texture_state(*m_device, m_cbcr,
                         gfx::resource_state::ShaderReadOnly);
  return true;
}

} // namespace nxm::video

#endif
