#pragma once

#include "video/video_source.h"

#include "core/foundation/strings/utf8_string_view.h"

namespace nxm::video {

[[nodiscard]] SourcePtr open_webm(nx::string_view path);

[[nodiscard]] bool webm_is_vp9(nx::string_view path);

}
