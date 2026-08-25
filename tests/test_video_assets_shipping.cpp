#include "framework/nxtest.h"

#include "video/video_asset.h"
#include "video/video_clip.h"
#include "video/video_demux.h"

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

[[nodiscard]] nx::blob<u8> fixture() {
  return nx::fs::file_read(nx::fs::path_view(NX_VIDEO_FIXTURE_DIR "/test.webm"))
      .value_or(nx::blob<u8>{});
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

TEST_CASE("video assets: Shipping rejects authored-only clip and media") {
  MountedMemory files;
  REQUIRE(files.ok);
  files.device->add(
      "/demo.nxvid",
      blob_of(R"({"version":1,"source":"/movie.webm","loop":true})"));
  nx::blob<u8> source = fixture();
  REQUIRE_FALSE(source.empty());
  files.device->add("/movie.webm", std::move(source));

  nxm::video::VideoClip clip;
  CHECK_FALSE(nxm::video::load_video_clip("/demo.nxvid", clip));
  nxm::video::EncodedVideo encoded;
  CHECK_FALSE(nxm::video::read_video_file("/movie.webm", encoded));
}

TEST_CASE("video assets: Shipping accepts both cooked containers") {
  MountedMemory files;
  REQUIRE(files.ok);
  const nxm::video::VideoClip source_clip{"/movie.webm", false};
  const nx::vector<u8> clip = nxm::video::encode_video_clip(source_clip);
  files.device->add("/demo.nxvid.nxb", blob_of({clip.data(), clip.size()}));

  const nx::blob<u8> source = fixture();
  REQUIRE_FALSE(source.empty());
  nx::string error;
  const auto media =
      nxm::video::encode_video_media({source.data(), source.size()}, error);
  REQUIRE(media.has_value());
  files.device->add("/movie.webm.nxb", blob_of({media->data(), media->size()}));

  nxm::video::VideoClip decoded_clip;
  REQUIRE(nxm::video::load_video_clip("/demo.nxvid", decoded_clip));
  CHECK(decoded_clip.source == source_clip.source);
  CHECK_FALSE(decoded_clip.loop);

  nxm::video::EncodedVideo decoded_media;
  REQUIRE(nxm::video::read_video_file("/movie.webm", decoded_media));
  CHECK(decoded_media.size() == source.size());
}

TEST_CASE("video assets: Shipping never hides malformed cooked siblings") {
  MountedMemory files;
  REQUIRE(files.ok);
  files.device->add(
      "/demo.nxvid",
      blob_of(R"({"version":1,"source":"/movie.webm","loop":true})"));
  files.device->add("/demo.nxvid.nxb", nx::blob<u8>{});
  nx::blob<u8> source = fixture();
  REQUIRE_FALSE(source.empty());
  files.device->add("/movie.webm", std::move(source));
  files.device->add("/movie.webm.nxb", nx::blob<u8>{});

  nxm::video::VideoClip clip;
  CHECK_FALSE(nxm::video::load_video_clip("/demo.nxvid", clip));
  nxm::video::EncodedVideo encoded;
  CHECK_FALSE(nxm::video::read_video_file("/movie.webm", encoded));
}
