#pragma once

#include "core/foundation/reflection/attributes.h"
#include "core/foundation/strings/utf8_string.h"
#include "core/foundation/strings/utf8_string_view.h"

namespace nxm::video {

struct VideoClip {
  nx::string source;
  bool loop = true;
};

/// Reads a .nxvid file into @p out. False if it cannot be read or parsed.
[[nodiscard]] bool load_video_clip(nx::string_view path, VideoClip &out);

}
