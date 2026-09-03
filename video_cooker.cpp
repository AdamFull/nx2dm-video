#include "assetc/cooker_registry.h"

#include "video/video_asset.h"
#include "video/video_clip.h"

#include "core/foundation/platform/filesystem.h"
#include "core/foundation/serialization/json_document.h"

#include <cstdio>

namespace assetc {
namespace {

namespace fs = nx::fs;

inline constexpr nx::asset_contract::Format VIDEO_FORMATS[] = {
    {".nxvid", ".nxvid.nxb", nxm::video::CLIP_FORMAT},
    {".webm", ".webm.nxb", nxm::video::MEDIA_FORMAT},
};

[[nodiscard]] bool write(const CookContext &context,
                         const nx::vector<u8> &bytes) {
  return !bytes.empty() && static_cast<bool>(fs::file_write_atomic(
                               context.output, {bytes.data(), bytes.size()}));
}

[[nodiscard]] bool cook_clip(const CookContext &context, CookOutputs &) {
  const auto text = fs::file_read_text(context.source);
  if (!text)
    return false;
  const auto normalized = nx::json::normalize_asset_document(text->view());
  if (!normalized)
    return false;
  nxm::video::VideoClip clip;
  nx::string error;
  if (!nxm::video::parse_video_clip(normalized->view(), clip, &error)) {
    std::fprintf(stderr, "assetc: video clip '%.*s' is invalid: %.*s\n",
                 static_cast<int>(context.source.size()), context.source.data(),
                 static_cast<int>(error.size()), error.data());
    return false;
  }
  return write(context, nxm::video::encode_video_clip(clip));
}

[[nodiscard]] bool cook_media(const CookContext &context, CookOutputs &) {
  const auto encoded = fs::file_read(context.source);
  if (!encoded)
    return false;
  nx::string error;
  const auto cooked =
      nxm::video::encode_video_media({encoded->data(), encoded->size()}, error);
  if (!cooked) {
    std::fprintf(stderr, "assetc: WebM '%.*s' is invalid: %.*s\n",
                 static_cast<int>(context.source.size()), context.source.data(),
                 static_cast<int>(error.size()), error.data());
    return false;
  }
  return write(context, cooked.value());
}

[[nodiscard]] bool cook_video(const CookContext &context,
                              CookOutputs &outputs) {
  if (context.source.ends_with(".nxvid"))
    return cook_clip(context, outputs);
  if (context.source.ends_with(".webm"))
    return cook_media(context, outputs);
  return false;
}

} // namespace

bool nx_assetc_register_video(CookerRegistry &registry, nx::string &error) {
  return registry.add({"video", VIDEO_FORMATS, 1, cook_video, nullptr, {}},
                      error);
}

} // namespace assetc
