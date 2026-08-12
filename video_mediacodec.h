#pragma once

/**
 * @file video_mediacodec.h
 * @brief Android hardware decode via MediaCodec (namespace nxm::video).
 */

#include "video/video_source.h"

#include "core/foundation/strings/utf8_string_view.h"

namespace nxm::video {

[[nodiscard]] SourcePtr open_media_codec(nx::string_view path);

} // namespace nxm::video
