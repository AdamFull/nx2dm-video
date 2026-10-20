#pragma once

#include "core/foundation/containers/small_vector.h"
#include "nx/gfx/types.h"
#include "nx/gfx/vulkan.h"

namespace nxm::video {

enum class VideoCodec : u8 { VP9, AV1, Count };

struct VideoDecodeCaps {
  bool supported = false;
  u32 max_width = 0;
  u32 max_height = 0;
  u32 max_dpb_slots = 0;
  bool dpb_and_output_coincide = false;
};

[[nodiscard]] VideoDecodeCaps
query_video_decode_caps(const nx::gfx::device &device, VideoCodec codec);

/// The queue family of @p adapter that decodes VP9 and draws nothing, or ~0u
/// where the device has none or lacks the extensions and the feature.
[[nodiscard]] u32 find_vp9_decode_family(VkPhysicalDevice adapter);

struct VideoImage {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct DecodedPicture {
  bool ok = false;
  u32 slot = 0;
  bool show = false;
  u32 width = 0;
  u32 height = 0;
};

class VideoDecoder {
public:
  VideoDecoder() = default;
  ~VideoDecoder();

  VideoDecoder(const VideoDecoder &) = delete;
  VideoDecoder &operator=(const VideoDecoder &) = delete;

  /// Decodes on the queue @p device made for video_decode_client(); false
  /// where it made none.
  [[nodiscard]] bool init(nx::gfx::device &device, VideoCodec codec, u32 width,
                          u32 height);
  void shutdown() noexcept;

  [[nodiscard]] bool ready() const noexcept {
    return m_session != VK_NULL_HANDLE;
  }
  [[nodiscard]] VkExtent2D extent() const noexcept { return m_extent; }
  [[nodiscard]] u32 dpb_slots() const noexcept { return m_max_dpb_slots; }

  [[nodiscard]] u32 dpb_image_count() const noexcept {
    return nx::cast<u32>(m_dpb.size());
  }
  [[nodiscard]] VkImageView dpb_view(u32 slot) const noexcept {
    return slot < m_dpb.size() ? m_dpb[slot].view : VK_NULL_HANDLE;
  }

  [[nodiscard]] bool decode_frame(const u8 *frame, u32 size, bool &shown,
                                  u32 &width, u32 &height);
  [[nodiscard]] bool show_last();
  [[nodiscard]] nx::gfx::texture_handle luma_texture() const noexcept {
    return m_y;
  }
  [[nodiscard]] nx::gfx::texture_handle chroma_texture() const noexcept {
    return m_cbcr;
  }
  [[nodiscard]] nx::gfx::extent2d coded_extent() const noexcept {
    return {m_extent.width, m_extent.height};
  }

  void reset_stream() noexcept;

private:
  [[nodiscard]] bool decode(const u8 *frame, u32 size, DecodedPicture &out);
  [[nodiscard]] bool present(u32 slot);
  [[nodiscard]] bool create_command_resources();
  [[nodiscard]] bool
  create_bitstream_buffer(u64 capacity,
                          const VkVideoProfileListInfoKHR &profiles);
  [[nodiscard]] bool create_sampleable_image(const u32 *families,
                                             u32 family_count);
  [[nodiscard]] bool create_plane_resources();
  [[nodiscard]] u32 allocate_slot() noexcept;

  nx::gfx::device *m_device = nullptr;
  VkQueue m_video_queue = VK_NULL_HANDLE;
  u32 m_video_family = ~0u;
  VkVideoSessionKHR m_session = VK_NULL_HANDLE;
  nx::small_vector<VkDeviceMemory, 4> m_memories;
  nx::small_vector<VideoImage, 16> m_dpb;
  VideoImage m_output;
  VideoImage m_sampleable;
  bool m_coincide = false;
  VkFormat m_picture_format = VK_FORMAT_UNDEFINED;
  VkFormat m_dpb_format = VK_FORMAT_UNDEFINED;
  VkExtent2D m_extent = {};
  VkExtent2D m_coded_limit = {};
  u32 m_max_dpb_slots = 0;

  VkCommandPool m_pool = VK_NULL_HANDLE;
  VkCommandBuffer m_cmd = VK_NULL_HANDLE;
  VkFence m_fence = VK_NULL_HANDLE;
  bool m_video_supports_transfer = false;
  bool m_needs_reset = true;

  VkBuffer m_bitstream = VK_NULL_HANDLE;
  VkDeviceMemory m_bitstream_memory = VK_NULL_HANDLE;
  void *m_bitstream_mapped = nullptr;
  u64 m_bitstream_capacity = 0;
  u64 m_bitstream_offset_align = 1;
  u64 m_bitstream_size_align = 1;

  u32 m_vp9_to_dpb[8] = {};
  bool m_vp9_valid[8] = {};
  u32 m_slot_width[16] = {};
  u32 m_slot_height[16] = {};
  u32 m_next_slot = 0;
  u32 m_last_slot = 0;
  bool m_have_last = false;

  VkCommandPool m_gfx_pool = VK_NULL_HANDLE;
  VkCommandBuffer m_gfx_cmd = VK_NULL_HANDLE;
  VkFence m_gfx_fence = VK_NULL_HANDLE;
  nx::gfx::texture_handle m_y;
  nx::gfx::texture_handle m_cbcr;
};

} // namespace nxm::video
