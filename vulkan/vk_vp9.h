#pragma once

#include "rendering/rhi/vulkan/vk_common.h"

namespace nxm::video {

struct Vp9FrameHeader {
  bool valid = false;

  bool show_existing_frame = false;
  u8 frame_to_show_map_idx = 0;

  StdVideoDecodeVP9PictureInfo picture{};
  StdVideoVP9ColorConfig color{};
  StdVideoVP9LoopFilter loop_filter{};
  StdVideoVP9Segmentation segmentation{};

  u32 frame_width = 0;
  u32 frame_height = 0;
  u8 ref_frame_idx[3] = {0, 0, 0};
  u32 uncompressed_header_size = 0;
  u32 compressed_header_size = 0;
};

[[nodiscard]] bool parse_vp9_frame_header(const u8 *data, u32 size,
                                          const u32 ref_width[8],
                                          const u32 ref_height[8],
                                          Vp9FrameHeader &out);

} // namespace nxm::video
