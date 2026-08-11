#pragma once

/**
 * @file video_audio.h
 * @brief The audio side of a .webm: an Opus decoder for the mixer
 * (namespace nxm::video).
 */

#include "core/audio/decoder.h"
#include "core/foundation/strings/utf8_string_view.h"

namespace nxm::video {

/// Opens an Opus decoder over the audio track of the .webm at @p path, read
/// through the VFS. Null if the file is missing, carries no Opus track, or the
/// track has more channels than the mixer takes. This is what the video's sound
/// is played through, and its clock is what the picture chases.
[[nodiscard]] nxe::audio::DecoderPtr open_webm_opus(nx::string_view path);

} // namespace nxm::video
