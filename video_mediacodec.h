#pragma once

/**
 * @file video_mediacodec.h
 * @brief Android hardware decode via MediaCodec (namespace nxm::video).
 */

#include "video/video_source.h"

#include "core/foundation/strings/utf8_string_view.h"

namespace nxm::video {

namespace mediacodec_detail {

[[nodiscard]] bool copy_plane_checked(u8 *dst, usize dst_size, u32 dst_pitch,
                                      const u8 *src, usize src_size,
                                      i32 src_row_stride, i32 src_pixel_stride,
                                      u32 width, u32 height) noexcept;

} // namespace mediacodec_detail

[[nodiscard]] SourcePtr open_media_codec(nx::string_view path);

} // namespace nxm::video
