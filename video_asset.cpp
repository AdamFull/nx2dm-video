#include "video/video_asset.h"

#include "video/video_webm.h"

#include <utility>

namespace nxm::video {
namespace {

constexpr u32 CHUNK_META = nx::nva::fourcc('M', 'E', 'T', 'A');
constexpr u32 CHUNK_DATA = nx::nva::fourcc('D', 'A', 'T', 'A');

[[nodiscard]] bool ends_with(const nx::string_view value,
                             const nx::string_view suffix) noexcept {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

} // namespace

bool valid_video_dimensions(const u64 width, const u64 height) noexcept {
  if (width == 0 || height == 0 || width > MAX_VIDEO_DIMENSION ||
      height > MAX_VIDEO_DIMENSION)
    return false;
  const u64 luma = width * height;
  const u64 chroma_width = width / 2 + width % 2;
  const u64 chroma_height = height / 2 + height % 2;
  const u64 chroma = chroma_width * chroma_height;
  return luma <= MAX_DECODED_VIDEO_FRAME_BYTES &&
         chroma <= (MAX_DECODED_VIDEO_FRAME_BYTES - luma) / 2;
}

bool valid_video_source_path(const nx::string_view path) noexcept {
  if (path.size() < 7 || path.size() > MAX_VIDEO_ASSET_PATH_BYTES ||
      path.front() != '/' || !ends_with(path, ".webm"))
    return false;

  usize segment_begin = 1;
  for (usize i = 1; i <= path.size(); ++i) {
    const bool boundary = i == path.size() || path[i] == '/';
    if (!boundary) {
      const unsigned char c = static_cast<unsigned char>(path[i]);
      if (c == 0 || c < 0x20 || path[i] == '\\')
        return false;
      continue;
    }
    const nx::string_view segment =
        path.substr(segment_begin, i - segment_begin);
    if (segment.empty() || segment == "." || segment == "..")
      return false;
    segment_begin = i + 1;
  }
  return true;
}

nx::vector<u8> encode_video_clip(const VideoClip &clip) {
  if (!valid_video_source_path(clip.source.view()))
    return {};
  nx::nva::Writer data;
  data.str(clip.source.view());
  data.u8v(clip.loop ? 1 : 0);

  nx::nva::ChunkWriter container(CLIP_FORMAT, VIDEO_ASSET_VERSION);
  container.add(CHUNK_DATA, data.span());
  return container.finish();
}

bool decode_video_clip(const std::span<const u8> bytes, VideoClip &out) {
  try {
    const std::optional<nx::nva::View> view =
        nx::nva::View::open(bytes, CLIP_FORMAT);
    if (!view || view->version() != VIDEO_ASSET_VERSION || view->flags() != 0)
      return false;
    const std::span<const u8> data = view->chunk(CHUNK_DATA);
    if (data.empty())
      return false;

    nx::nva::Reader reader(data);
    VideoClip decoded;
    decoded.source = nx::string(reader.str());
    const u8 loop = reader.u8v();
    if (!reader.ok() || reader.remaining() != 0 || loop > 1 ||
        !valid_video_source_path(decoded.source.view()))
      return false;
    decoded.loop = loop != 0;
    out = std::move(decoded);
    return true;
  } catch (...) {
    return false;
  }
}

std::optional<nx::vector<u8>>
encode_video_media(const std::span<const u8> encoded, nx::string &error) {
  if (!validate_webm(encoded, error))
    return std::nullopt;
  try {
    nx::nva::Writer metadata;
    metadata.u64v(nx::cast<u64>(encoded.size()));

    nx::nva::ChunkWriter container(MEDIA_FORMAT, VIDEO_ASSET_VERSION);
    container.add(CHUNK_META, metadata.span());
    container.add(CHUNK_DATA, encoded);
    return container.finish();
  } catch (...) {
    error = "resource exhaustion while wrapping WebM";
    return std::nullopt;
  }
}

bool decode_video_media(nx::blob<u8> container, EncodedVideo &out) noexcept {
  if (container.size() > MAX_COOKED_VIDEO_BYTES)
    return false;
  try {
    const std::span<const u8> bytes(container.data(), container.size());
    const std::optional<nx::nva::View> view =
        nx::nva::View::open(bytes, MEDIA_FORMAT);
    if (!view || view->version() != VIDEO_ASSET_VERSION || view->flags() != 0)
      return false;
    const std::span<const u8> metadata = view->chunk(CHUNK_META);
    const std::span<const u8> payload = view->chunk(CHUNK_DATA);
    if (metadata.empty() || payload.empty() ||
        payload.size() > MAX_ENCODED_VIDEO_BYTES)
      return false;

    nx::nva::Reader reader(metadata);
    const u64 declared_size = reader.u64v();
    if (!reader.ok() || reader.remaining() != 0 ||
        declared_size != nx::cast<u64>(payload.size()))
      return false;

    const usize offset = nx::cast<usize>(payload.data() - bytes.data());
    EncodedVideo decoded;
    decoded.storage = std::move(container);
    decoded.payload_offset = offset;
    decoded.payload_size = payload.size();
    out = std::move(decoded);
    return true;
  } catch (...) {
    return false;
  }
}

nx::string cooked_video_path(const nx::string_view source) {
  nx::string out(source);
  out += ".nxb";
  return out;
}

} // namespace nxm::video
