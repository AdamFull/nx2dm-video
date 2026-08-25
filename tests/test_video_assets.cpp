#include "framework/nxtest.h"

#include "video/video_asset.h"
#include "video/video_clip.h"
#include "video/video_demux.h"
#include "video/video_webm.h"

#include "core/foundation/platform/filesystem.h"
#include "core/foundation/vfs/vfs.h"

#include <span>

namespace {

[[nodiscard]] nx::blob<u8> blob_of(const std::span<const u8> bytes) {
  nx::blob<u8> out(bytes.size());
  for (usize i = 0; i < bytes.size(); ++i)
    out[i] = bytes[i];
  return out;
}

[[nodiscard]] nx::blob<u8> blob_of(const nx::string_view text) {
  return blob_of({reinterpret_cast<const u8 *>(text.data()), text.size()});
}

[[nodiscard]] nx::blob<u8> fixture(const char *const name) {
  nx::string path(NX_VIDEO_FIXTURE_DIR);
  path += "/";
  path += name;
  const auto bytes = nx::fs::file_read(path.view());
  return bytes ? std::move(bytes.value()) : nx::blob<u8>{};
}

[[nodiscard]] bool same(const std::span<const u8> a,
                        const std::span<const u8> b) {
  if (a.size() != b.size())
    return false;
  for (usize i = 0; i < a.size(); ++i)
    if (a[i] != b[i])
      return false;
  return true;
}

struct MountedMemory {
  nx::vfs::MemoryDevice *device = nullptr;
  bool ok = false;

  MountedMemory() {
    if (!nx::vfs::initialize())
      return;
    device = nx::vfs::make_memory_device();
    ok = device != nullptr && nx::vfs::mount("/", device, 200).valid();
  }
  ~MountedMemory() { nx::vfs::unmount_all(); }
};

} // namespace

TEST_CASE("video assets: authored clips use a strict versioned schema") {
  nxm::video::VideoClip clip;
  nx::string error;
  REQUIRE(nxm::video::parse_video_clip(
      R"({"version":1,"source":"/video/intro.webm","loop":false})", clip,
      &error));
  CHECK(clip.source == "/video/intro.webm");
  CHECK_FALSE(clip.loop);

  constexpr nx::string_view invalid[] = {
      R"({"source":"relative.webm"})",
      R"({"source":"/video/../intro.webm"})",
      R"({"source":"/video/intro.mp4"})",
      R"({"source":"/video/intro.webm","loop":1})",
      R"({"version":2,"source":"/video/intro.webm"})",
      R"({"source":"/video/intro.webm","extra":true})",
      R"({"version":1})",
      R"([])",
  };
  for (const nx::string_view document : invalid) {
    error.clear();
    CHECK_FALSE(nxm::video::parse_video_clip(document, clip, &error));
    CHECK_FALSE(error.empty());
  }
}

TEST_CASE("video assets: clip containers round-trip and reject corruption") {
  const nxm::video::VideoClip source{"/cinematics/opening.webm", false};
  nx::vector<u8> bytes = nxm::video::encode_video_clip(source);
  REQUIRE(bytes.size() > 16u);

  nxm::video::VideoClip decoded;
  REQUIRE(nxm::video::decode_video_clip({bytes.data(), bytes.size()}, decoded));
  CHECK(decoded.source == source.source);
  CHECK(decoded.loop == source.loop);

  bytes[4] = 'B';
  CHECK_FALSE(
      nxm::video::decode_video_clip({bytes.data(), bytes.size()}, decoded));
  CHECK_FALSE(nxm::video::decode_video_clip({}, decoded));
}

TEST_CASE(
    "video assets: supported WebM variants validate and wrap losslessly") {
  constexpr const char *fixtures[] = {
      "test.webm",     "test_audio.webm",      "test_vorbis.webm",
      "test_av1.webm", "test_bt601_full.webm",
  };
  for (const char *const name : fixtures) {
    const nx::blob<u8> source = fixture(name);
    REQUIRE_FALSE(source.empty());

    nx::string error;
    REQUIRE(nxm::video::validate_webm({source.data(), source.size()}, error));
    const auto wrapped =
        nxm::video::encode_video_media({source.data(), source.size()}, error);
    REQUIRE(wrapped.has_value());

    nxm::video::EncodedVideo decoded;
    REQUIRE(nxm::video::decode_video_media(
        blob_of({wrapped->data(), wrapped->size()}), decoded));
    CHECK(same(decoded.span(), {source.data(), source.size()}));
    CHECK(decoded.data() >= decoded.storage.data());
    CHECK(decoded.data() + decoded.size() <=
          decoded.storage.data() + decoded.storage.size());
  }

  const nx::blob<u8> complete = fixture("test.webm");
  REQUIRE(complete.size() > 32u);
  nx::blob<u8> truncated(32u);
  for (usize i = 0; i < truncated.size(); ++i)
    truncated[i] = complete[i];
  nx::string error;
  CHECK_FALSE(
      nxm::video::validate_webm({truncated.data(), truncated.size()}, error));
  CHECK_FALSE(error.empty());
}

TEST_CASE(
    "video assets: cooked siblings win and malformed ones block fallback") {
  MountedMemory files;
  REQUIRE(files.ok);

  files.device->add(
      "/demo.nxvid",
      blob_of(R"({"version":1,"source":"/raw.webm","loop":true})"));
  const nxm::video::VideoClip cooked_clip{"/cooked.webm", false};
  const nx::vector<u8> clip_bytes = nxm::video::encode_video_clip(cooked_clip);
  files.device->add("/demo.nxvid.nxb",
                    blob_of({clip_bytes.data(), clip_bytes.size()}));

  nxm::video::VideoClip clip;
  REQUIRE(nxm::video::load_video_clip("/demo.nxvid", clip));
  CHECK(clip.source == "/cooked.webm");
  CHECK_FALSE(clip.loop);

  const nx::blob<u8> source = fixture("test.webm");
  REQUIRE_FALSE(source.empty());
  files.device->add("/movie.webm", blob_of({source.data(), source.size()}));
  nx::string error;
  const auto media =
      nxm::video::encode_video_media({source.data(), source.size()}, error);
  REQUIRE(media.has_value());
  files.device->add("/movie.webm.nxb", blob_of({media->data(), media->size()}));

  nxm::video::EncodedVideo encoded;
  REQUIRE(nxm::video::read_video_file("/movie.webm", encoded));
  CHECK(same(encoded.span(), {source.data(), source.size()}));

  files.device->add("/demo.nxvid.nxb", nx::blob<u8>{});
  CHECK_FALSE(nxm::video::load_video_clip("/demo.nxvid", clip));
  files.device->add("/movie.webm.nxb", nx::blob<u8>{});
  CHECK_FALSE(nxm::video::read_video_file("/movie.webm", encoded));
}

TEST_CASE("video assets: development falls back only when cooked is absent") {
  MountedMemory files;
  REQUIRE(files.ok);

  files.device->add(
      "/demo.nxvid",
      blob_of(R"({"version":1,"source":"/movie.webm","loop":true})"));
  const nx::blob<u8> source = fixture("test.webm");
  REQUIRE_FALSE(source.empty());
  files.device->add("/movie.webm", blob_of({source.data(), source.size()}));

  nxm::video::VideoClip clip;
  REQUIRE(nxm::video::load_video_clip("/demo.nxvid", clip));
  CHECK(clip.source == "/movie.webm");

  nxm::video::EncodedVideo encoded;
  REQUIRE(nxm::video::read_video_file("/movie.webm", encoded));
  CHECK(same(encoded.span(), {source.data(), source.size()}));
}
