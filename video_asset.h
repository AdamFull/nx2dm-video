#pragma once

#include "core/foundation/containers/blob.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/serialization/nva.h"
#include "core/foundation/strings/utf8_string.h"
#include "core/foundation/strings/utf8_string_view.h"

#include <optional>
#include <span>

namespace nxm::video {

inline constexpr usize MAX_ENCODED_VIDEO_BYTES = 256u * 1024u * 1024u;
inline constexpr usize MAX_COOKED_VIDEO_BYTES =
    MAX_ENCODED_VIDEO_BYTES + 1u * 1024u * 1024u;
inline constexpr usize MAX_VIDEO_CLIP_BYTES = 64u * 1024u;
inline constexpr usize MAX_VIDEO_ASSET_PATH_BYTES = 4096;
inline constexpr usize MAX_VIDEO_PACKET_BYTES = 16u * 1024u * 1024u;
inline constexpr usize MAX_VIDEO_CODEC_PRIVATE_BYTES = 1u * 1024u * 1024u;
inline constexpr usize MAX_DECODED_VIDEO_FRAME_BYTES = 64u * 1024u * 1024u;
inline constexpr usize MAX_VIDEO_CODEC_FRAME_BUFFER_BYTES = 96u * 1024u * 1024u;
inline constexpr usize MAX_VIDEO_DECODER_BYTES = 256u * 1024u * 1024u;
inline constexpr u32 MAX_VIDEO_DIMENSION = 8192;
inline constexpr f64 MAX_VIDEO_FRAME_RATE = 240.0;

inline constexpr u32 CLIP_FORMAT = nx::nva::fourcc('N', 'X', 'V', 'D');
inline constexpr u32 MEDIA_FORMAT = nx::nva::fourcc('N', 'X', 'V', 'M');
inline constexpr u16 VIDEO_ASSET_VERSION = 1;

struct VideoClip {
  nx::string source;
  bool loop = true;
};

struct EncodedVideo {
  nx::blob<u8> storage;
  usize payload_offset = 0;
  usize payload_size = 0;

  [[nodiscard]] const u8 *data() const noexcept {
    return payload_size != 0 ? storage.data() + payload_offset : nullptr;
  }
  [[nodiscard]] usize size() const noexcept { return payload_size; }
  [[nodiscard]] bool empty() const noexcept { return payload_size == 0; }
  [[nodiscard]] std::span<const u8> span() const noexcept {
    return {data(), size()};
  }
};

[[nodiscard]] bool valid_video_dimensions(u64 width, u64 height) noexcept;
[[nodiscard]] bool valid_video_source_path(nx::string_view path) noexcept;

[[nodiscard]] nx::vector<u8> encode_video_clip(const VideoClip &clip);
[[nodiscard]] bool decode_video_clip(std::span<const u8> bytes, VideoClip &out);

/// Validates the WebM container and wraps its encoded bytes without
/// transcoding. The returned payload remains seekable by the runtime demuxer.
[[nodiscard]] std::optional<nx::vector<u8>>
encode_video_media(std::span<const u8> encoded, nx::string &error);

/// Opens a cooked media wrapper without copying its (potentially large) WebM
/// payload. EncodedVideo retains the complete container and exposes a subspan.
[[nodiscard]] bool decode_video_media(nx::blob<u8> container,
                                      EncodedVideo &out) noexcept;

[[nodiscard]] nx::string cooked_video_path(nx::string_view source);

} // namespace nxm::video
