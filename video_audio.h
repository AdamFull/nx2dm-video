#pragma once

#include "audio/decoder.h"
#include "core/foundation/strings/utf8_string_view.h"

namespace nxm::video {

[[nodiscard]] nxe::audio::DecoderPtr open_webm_opus(nx::string_view path);

[[nodiscard]] nxe::audio::DecoderPtr open_webm_vorbis(nx::string_view path);

[[nodiscard]] nxe::audio::DecoderPtr open_webm_audio(nx::string_view path);

}
