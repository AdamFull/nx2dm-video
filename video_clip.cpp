#include "video/video_clip.h"

#include "core/foundation/diagnostics/log.h"
#include "core/foundation/serialization/asset_policy.h"
#include "core/foundation/serialization/json.h"
#include "core/foundation/vfs/vfs.h"

#include <utility>

namespace nxm::video {
namespace {

[[nodiscard]] bool fail(nx::string *const error,
                        const nx::string_view message) {
  if (error != nullptr)
    *error = nx::string(message);
  return false;
}

[[nodiscard]] bool known_clip_field(const nx::string_view name) noexcept {
  return name == "version" || name == "source" || name == "loop";
}

} // namespace

bool parse_video_clip(const nx::string_view text, VideoClip &out,
                      nx::string *const error) {
  if (error != nullptr)
    error->clear();
  try {
    auto document = nx::json::parse(text);
    if (!document || !document.value().is_object())
      return fail(error, "the clip is not a JSON object");
    const nx::json::value &root = document.value();
    for (const nx::json::member &member : root.as_object())
      if (!known_clip_field(member.key.view()))
        return fail(error, "the clip contains an unknown field");

    if (const nx::json::value *const version = root.find("version")) {
      u64 number = 0;
      if (version->is_u64()) {
        number = version->as_u64();
      } else if (version->is_i64() && version->as_i64() >= 0) {
        number = nx::cast<u64>(version->as_i64());
      } else {
        return fail(error, "the clip version must be a non-negative integer");
      }
      if (number > VIDEO_ASSET_VERSION)
        return fail(error, "the clip version is newer than this build");
    }

    const nx::json::value *const source = root.find("source");
    if (source == nullptr || !source->is_string())
      return fail(error, "the clip must name a source string");

    VideoClip decoded;
    decoded.source = nx::string(source->as_string());
    if (!valid_video_source_path(decoded.source.view()))
      return fail(error,
                  "the clip source must be a canonical absolute .webm path");
    if (const nx::json::value *const loop = root.find("loop")) {
      if (!loop->is_bool())
        return fail(error, "the clip loop field must be boolean");
      decoded.loop = loop->as_bool();
    }
    out = std::move(decoded);
    return true;
  } catch (...) {
    return fail(error, "resource exhaustion while parsing the clip");
  }
}

bool load_video_clip(const nx::string_view path, VideoClip &out) {
  try {
    const nx::string cooked_path = cooked_video_path(path);
    const nx::vfs::FileInfo cooked_info = nx::vfs::stat(cooked_path.view());
    if (cooked_info.exists) {
      if (cooked_info.is_directory || cooked_info.size > MAX_VIDEO_CLIP_BYTES) {
        nx::logw("video: cooked clip '{}' is too large or not a file",
                 cooked_path);
        return false;
      }
      const auto bytes = nx::vfs::read(cooked_path.view());
      if (!bytes || bytes->size() > MAX_VIDEO_CLIP_BYTES ||
          !decode_video_clip({bytes->data(), bytes->size()}, out)) {
        nx::logw("video: cooked clip '{}' is malformed", cooked_path);
        return false;
      }
      return true;
    }
    if (!nx::asset_policy::can_fallback_to_authored_source(
            cooked_info.exists)) {
      nx::logw("video: no cooked clip at '{}'", cooked_path);
      return false;
    }

    const nx::vfs::FileInfo source_info = nx::vfs::stat(path);
    if (!source_info.exists || source_info.is_directory ||
        source_info.size > MAX_VIDEO_CLIP_BYTES) {
      nx::logw("video: cannot read clip '{}'", path);
      return false;
    }
    const auto text = nx::vfs::read_text(nx::vfs::path_view(path));
    nx::string error;
    if (!text || text->size() > MAX_VIDEO_CLIP_BYTES ||
        !parse_video_clip(text->view(), out, &error)) {
      nx::logw("video: authored clip '{}' is invalid: {}", path, error);
      return false;
    }
    return true;
  } catch (...) {
    nx::logw("video: exhausted resources while loading clip '{}'", path);
    return false;
  }
}

} // namespace nxm::video
