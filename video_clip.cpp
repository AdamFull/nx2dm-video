#include "video/video_clip.h"

#include "core/foundation/diagnostics/log.h"
#include "core/foundation/serialization/json.h"
#include "core/foundation/serialization/json_util.h"
#include "core/foundation/serialization/reflect_json.h"
#include "core/foundation/vfs/vfs.h"

namespace nxm::video {
namespace {

constexpr u64 FORMAT_VERSION = 1;

}

bool load_video_clip(const nx::string_view path, VideoClip &out) {
  auto text = nx::vfs::read_text(nx::vfs::path_view(path));
  if (!text) {
    nx::logw("video: cannot read clip '{}'", path);
    return false;
  }
  auto doc = nx::json::parse(text->view());
  if (!doc || !doc.value().is_object()) {
    nx::logw("video: '{}' is not a valid .nxvid", path);
    return false;
  }
  if (!nx::json::version_ok(doc.value(), FORMAT_VERSION)) {
    nx::logw("video: '{}' is a newer .nxvid than this build reads", path);
    return false;
  }
  nx::json::read_fields(doc.value(), out);
  if (out.source.empty()) {
    nx::logw("video: '{}' names no source", path);
    return false;
  }
  return true;
}

}
