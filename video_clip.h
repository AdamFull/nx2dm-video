#pragma once

#include "video/video_asset.h"

namespace nxm::video {

/// Parses the authored versioned JSON schema. Source paths are canonical VFS
/// paths to `.webm` assets; relative paths and traversal are rejected.
[[nodiscard]] bool parse_video_clip(nx::string_view text, VideoClip &out,
                                    nx::string *error = nullptr);

/// Prefers `<path>.nxb`. Development builds fall back to authored JSON only
/// when the cooked sibling is absent; Shipping builds never fall back.
[[nodiscard]] bool load_video_clip(nx::string_view path, VideoClip &out);

} // namespace nxm::video
