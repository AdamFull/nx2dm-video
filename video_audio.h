#pragma once

#include "audio/decoder.h"
#include "audio/stream.h"
#include "core/foundation/strings/utf8_string_view.h"

namespace nxm::video {

[[nodiscard]] nxe::audio::DecoderPtr open_webm_opus(nx::string_view path);

[[nodiscard]] nxe::audio::DecoderPtr open_webm_vorbis(nx::string_view path);

[[nodiscard]] nxe::audio::DecoderPtr open_webm_audio(nx::string_view path);

/// The audio track of @p path as a stream from @p start_seconds, with its
/// first buffer decoded: everything before the mixer plays it, so it may run
/// on any thread. Null when the clip has no audio it can decode.
[[nodiscard]] nx::unique_ptr<nxe::audio::AudioStream>
open_video_audio(nx::string_view path, bool looping, f64 start_seconds);
}
