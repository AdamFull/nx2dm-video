
#include "framework/nxtest.h"

#include "video/video_hw.h"

#include "rendering/rhi/device.h"
#include "rendering/rhi/vulkan/vk_video.h"
#include "rendering/rhi/vulkan/vk_vp9.h"

#include "core/foundation/vfs/vfs.h"

#include "mkvparser/mkvparser.h"

#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"
#include "vpx/vpx_image.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

namespace rhi = nxe::rhi;

class MemoryReader final : public mkvparser::IMkvReader {
public:
  MemoryReader(const u8 *data, long long size) noexcept
      : m_data(data), m_size(size) {}
  int Read(long long pos, long len, unsigned char *buf) override {
    if (pos < 0 || len < 0 || pos + len > m_size)
      return -1;
    std::memcpy(buf, m_data + pos, static_cast<size_t>(len));
    return 0;
  }
  int Length(long long *total, long long *available) override {
    if (total != nullptr)
      *total = m_size;
    if (available != nullptr)
      *available = m_size;
    return 0;
  }

private:
  const u8 *m_data;
  long long m_size;
};

struct RawFrame {
  std::vector<u8> bytes;
  bool key = false;
};

[[nodiscard]] std::vector<RawFrame> read_frames(const char *path,
                                                std::vector<u8> &file_backing,
                                                const usize want) {
  std::vector<RawFrame> frames;
  std::FILE *fp = std::fopen(path, "rb");
  if (fp == nullptr)
    return frames;
  std::fseek(fp, 0, SEEK_END);
  const long size = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);
  file_backing.resize(static_cast<usize>(size));
  if (std::fread(file_backing.data(), 1, static_cast<usize>(size), fp) !=
      static_cast<usize>(size)) {
    std::fclose(fp);
    return frames;
  }
  std::fclose(fp);

  static MemoryReader reader(nullptr, 0);
  reader = MemoryReader(file_backing.data(), size);

  long long pos = 0;
  if (mkvparser::EBMLHeader{}.Parse(&reader, pos) < 0)
    return frames;
  mkvparser::Segment *segment = nullptr;
  if (mkvparser::Segment::CreateInstance(&reader, pos, segment) < 0 ||
      segment == nullptr || segment->Load() < 0)
    return frames;

  const mkvparser::Tracks *tracks = segment->GetTracks();
  long long track_number = 0;
  for (unsigned long i = 0; tracks != nullptr && i < tracks->GetTracksCount();
       ++i) {
    const mkvparser::Track *track = tracks->GetTrackByIndex(i);
    if (track != nullptr && track->GetType() == mkvparser::Track::kVideo &&
        track->GetCodecId() != nullptr &&
        std::strcmp(track->GetCodecId(), "V_VP9") == 0) {
      track_number = track->GetNumber();
      break;
    }
  }

  for (const mkvparser::Cluster *cluster = segment->GetFirst();
       cluster != nullptr && !cluster->EOS() && frames.size() < want;
       cluster = segment->GetNext(cluster)) {
    const mkvparser::BlockEntry *entry = nullptr;
    long status = cluster->GetFirst(entry);
    while (status >= 0 && entry != nullptr && !entry->EOS() &&
           frames.size() < want) {
      const mkvparser::Block *block = entry->GetBlock();
      if (block != nullptr && block->GetTrackNumber() == track_number &&
          block->GetFrameCount() > 0) {
        const mkvparser::Block::Frame &f = block->GetFrame(0);
        if (f.len > 0) {
          RawFrame raw;
          raw.bytes.resize(static_cast<usize>(f.len));
          raw.key = block->IsKey();
          if (f.Read(&reader, raw.bytes.data()) >= 0)
            frames.push_back(std::move(raw));
        }
      }
      const mkvparser::BlockEntry *next = nullptr;
      status = cluster->GetNext(entry, next);
      entry = next;
    }
  }

  delete segment;
  return frames;
}

struct VpxDims {
  bool ok = false;
  u32 width = 0;
  u32 height = 0;
};

[[nodiscard]] VpxDims vpx_decode_dims(const std::vector<RawFrame> &frames,
                                      const usize upto) {
  VpxDims dims;
  vpx_codec_ctx_t codec{};
  if (vpx_codec_dec_init(&codec, vpx_codec_vp9_dx(), nullptr, 0) !=
      VPX_CODEC_OK)
    return dims;
  for (usize i = 0; i <= upto && i < frames.size(); ++i) {
    if (vpx_codec_decode(&codec, frames[i].bytes.data(),
                         static_cast<unsigned int>(frames[i].bytes.size()),
                         nullptr, 0) != VPX_CODEC_OK)
      break;
    vpx_codec_iter_t it = nullptr;
    if (const vpx_image_t *img = vpx_codec_get_frame(&codec, &it)) {
      dims.ok = true;
      dims.width = img->d_w;
      dims.height = img->d_h;
    }
  }
  vpx_codec_destroy(&codec);
  return dims;
}

struct VpxLuma {
  bool ok = false;
  u32 width = 0;
  u32 height = 0;
  std::vector<u8> y;
};

[[nodiscard]] VpxLuma vpx_decode_luma(const std::vector<RawFrame> &frames,
                                      const usize upto) {
  VpxLuma luma;
  vpx_codec_ctx_t codec{};
  if (vpx_codec_dec_init(&codec, vpx_codec_vp9_dx(), nullptr, 0) !=
      VPX_CODEC_OK)
    return luma;
  for (usize i = 0; i <= upto && i < frames.size(); ++i) {
    if (vpx_codec_decode(&codec, frames[i].bytes.data(),
                         static_cast<unsigned int>(frames[i].bytes.size()),
                         nullptr, 0) != VPX_CODEC_OK)
      break;
    vpx_codec_iter_t it = nullptr;
    if (const vpx_image_t *img = vpx_codec_get_frame(&codec, &it)) {
      luma.ok = true;
      luma.width = img->d_w;
      luma.height = img->d_h;
      luma.y.resize(static_cast<usize>(img->d_w) * img->d_h);
      const u8 *src = img->planes[VPX_PLANE_Y];
      for (u32 row = 0; row < img->d_h; ++row)
        std::memcpy(luma.y.data() + static_cast<usize>(row) * img->d_w,
                    src + static_cast<usize>(row) * img->stride[VPX_PLANE_Y],
                    img->d_w);
    }
  }
  vpx_codec_destroy(&codec);
  return luma;
}

struct TestDevice {
  nxe::rhi::Device device;
  bool ready = false;
  TestDevice() {
    nxe::rhi::DeviceDesc desc{};
    desc.application_name = "nx video hw decode tests";
    ready = device.init(desc);
  }
  ~TestDevice() {
    if (ready)
      device.shutdown();
  }
  TestDevice(const TestDevice &) = delete;
  TestDevice &operator=(const TestDevice &) = delete;
};

}

TEST_CASE("vp9 parse: the keyframe header agrees with libvpx on size and type") {
  std::vector<u8> backing;
  const std::vector<RawFrame> frames =
      read_frames(NX_VIDEO_FIXTURE_DIR "/test.webm", backing, 6);
  REQUIRE(frames.size() >= 2);
  REQUIRE(frames[0].key);

  rhi::Vp9FrameHeader header;
  REQUIRE(rhi::parse_vp9_frame_header(frames[0].bytes.data(),
                                      static_cast<u32>(frames[0].bytes.size()),
                                      nullptr, nullptr, header));
  CHECK(header.valid);
  CHECK(header.picture.frame_type == STD_VIDEO_VP9_FRAME_TYPE_KEY);
  CHECK(header.picture.profile == STD_VIDEO_VP9_PROFILE_0);
  CHECK(header.picture.flags.show_frame == 1u);

  const VpxDims dims = vpx_decode_dims(frames, 0);
  REQUIRE(dims.ok);
  CHECK(dims.width == 320u);
  CHECK(dims.height == 240u);
  CHECK(header.frame_width == dims.width);
  CHECK(header.frame_height == dims.height);

  CHECK(header.uncompressed_header_size > 0u);
  CHECK(header.compressed_header_size > 0u);
  CHECK(header.uncompressed_header_size + header.compressed_header_size <
        frames[0].bytes.size());

  CHECK(header.color.BitDepth == 8u);
  CHECK(header.color.subsampling_x == 1u);
  CHECK(header.color.subsampling_y == 1u);
}

TEST_CASE("vp9 parse: an inter frame parses as non-key with a whole header") {
  std::vector<u8> backing;
  const std::vector<RawFrame> frames =
      read_frames(NX_VIDEO_FIXTURE_DIR "/test.webm", backing, 6);
  REQUIRE(frames.size() >= 2);

  rhi::Vp9FrameHeader key;
  REQUIRE(rhi::parse_vp9_frame_header(frames[0].bytes.data(),
                                      static_cast<u32>(frames[0].bytes.size()),
                                      nullptr, nullptr, key));
  u32 ref_w[8];
  u32 ref_h[8];
  for (u32 i = 0; i < 8; ++i) {
    ref_w[i] = key.frame_width;
    ref_h[i] = key.frame_height;
  }

  usize inter = 0;
  for (usize i = 1; i < frames.size(); ++i)
    if (!frames[i].key) {
      inter = i;
      break;
    }
  REQUIRE(inter != 0);

  rhi::Vp9FrameHeader header;
  REQUIRE(rhi::parse_vp9_frame_header(
      frames[inter].bytes.data(),
      static_cast<u32>(frames[inter].bytes.size()), ref_w, ref_h, header));
  CHECK(header.valid);
  CHECK(header.picture.frame_type == STD_VIDEO_VP9_FRAME_TYPE_NON_KEY);

  CHECK(header.compressed_header_size > 0u);
  CHECK(header.uncompressed_header_size + header.compressed_header_size <
        frames[inter].bytes.size());
}

TEST_CASE("vp9 parse: every frame of the clip parses, header inside the frame") {
  std::vector<u8> backing;
  const std::vector<RawFrame> frames =
      read_frames(NX_VIDEO_FIXTURE_DIR "/test.webm", backing, 1000);
  REQUIRE(frames.size() >= 40);

  u32 ref_w[8] = {};
  u32 ref_h[8] = {};
  for (const RawFrame &frame : frames) {
    rhi::Vp9FrameHeader header;
    REQUIRE(rhi::parse_vp9_frame_header(frame.bytes.data(),
                                        static_cast<u32>(frame.bytes.size()),
                                        ref_w, ref_h, header));
    CHECK(header.uncompressed_header_size + header.compressed_header_size <
          frame.bytes.size());

    if (header.frame_width != 0) {
      for (u32 i = 0; i < 8; ++i)
        if (header.picture.refresh_frame_flags & (1u << i)) {
          ref_w[i] = header.frame_width;
          ref_h[i] = header.frame_height;
        }
    }
  }
}

TEST_CASE("vp9 planes: the resolved luma and chroma match libvpx") {
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");
  if (!fixture.device.video_decode_available())
    SKIP("no hardware video-decode queue");

  std::vector<u8> backing;
  const std::vector<RawFrame> frames =
      read_frames(NX_VIDEO_FIXTURE_DIR "/test.webm", backing, 2);
  REQUIRE(!frames.empty());
  REQUIRE(frames[0].key);

  nxe::rhi::VideoDecoder decoder;
  REQUIRE(decoder.init(fixture.device, nxe::rhi::VideoCodec::VP9, 320, 240));

  bool shown = false;
  u32 w = 0;
  u32 h = 0;
  REQUIRE(decoder.decode_frame(frames[0].bytes.data(),
                               static_cast<u32>(frames[0].bytes.size()), shown,
                               w, h));
  REQUIRE(w == 320u);
  REQUIRE(h == 240u);
  REQUIRE(decoder.show_last());

  const u32 coded = decoder.coded_extent().width;
  const nxe::rhi::ReadbackResult luma =
      fixture.device.uploader().read_texture(decoder.luma_texture());
  const nxe::rhi::ReadbackResult chroma =
      fixture.device.uploader().read_texture(decoder.chroma_texture());
  REQUIRE(luma.data != nullptr);
  REQUIRE(chroma.data != nullptr);
  fixture.device.uploader().wait(luma.ticket);
  fixture.device.uploader().wait(chroma.ticket);

  const VpxLuma ref = vpx_decode_luma(frames, 0);
  REQUIRE(ref.ok);

  usize luma_mismatch = 0;
  for (u32 y = 0; y < h; ++y)
    for (u32 x = 0; x < w; ++x)
      if (luma.data[y * coded + x] != ref.y[y * w + x])
        ++luma_mismatch;
  CHECK(luma_mismatch == 0u);

  vpx_codec_ctx_t codec{};
  REQUIRE(vpx_codec_dec_init(&codec, vpx_codec_vp9_dx(), nullptr, 0) ==
          VPX_CODEC_OK);
  REQUIRE(vpx_codec_decode(&codec, frames[0].bytes.data(),
                           static_cast<unsigned int>(frames[0].bytes.size()),
                           nullptr, 0) == VPX_CODEC_OK);
  vpx_codec_iter_t it = nullptr;
  const vpx_image_t *img = vpx_codec_get_frame(&codec, &it);
  REQUIRE(img != nullptr);

  const u32 cw = w / 2;
  const u32 ch = h / 2;
  const u32 c_pitch = coded / 2;
  usize chroma_mismatch = 0;
  for (u32 y = 0; y < ch; ++y)
    for (u32 x = 0; x < cw; ++x) {
      const u8 cb = chroma.data[(y * c_pitch + x) * 2 + 0];
      const u8 cr = chroma.data[(y * c_pitch + x) * 2 + 1];
      if (cb != img->planes[VPX_PLANE_U][y * img->stride[VPX_PLANE_U] + x] ||
          cr != img->planes[VPX_PLANE_V][y * img->stride[VPX_PLANE_V] + x])
        ++chroma_mismatch;
    }
  vpx_codec_destroy(&codec);
  CHECK(chroma_mismatch == 0u);
}

TEST_CASE("hw source: a webm clip decodes and paces on the hardware path") {
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");
  if (!nxm::video::hw_decode_available(fixture.device))
    SKIP("no hardware video-decode queue");

  REQUIRE(nx::vfs::initialize());
  struct Unmount {
    ~Unmount() { nx::vfs::unmount_all(); }
  } unmount;
  REQUIRE(nx::vfs::mount("/", nx::vfs::make_host_device(NX_VIDEO_FIXTURE_DIR), 0)
              .valid());

  std::vector<u8> backing;
  const std::vector<RawFrame> frames =
      read_frames(NX_VIDEO_FIXTURE_DIR "/test.webm", backing, 2);
  REQUIRE(!frames.empty());

  nxm::video::HwVideoSource src;
  REQUIRE(src.open(fixture.device, "/test.webm"));
  CHECK(src.width() == 320u);
  CHECK(src.height() == 240u);
  CHECK(src.frame_rate() > 0.0);

  const nxm::video::GpuFrame first = src.frame_at(0.0, false);
  REQUIRE(first.valid());
  CHECK(src.position() < 0.2);
  CHECK(first.uv_scale.x > 0.9f);
  CHECK(first.uv_scale.y > 0.9f);

  const nxm::video::GpuFrame later = src.frame_at(1.0, false);
  REQUIRE(later.valid());
  CHECK(src.position() > 0.8);
  CHECK(src.position() < 1.2);
  CHECK_FALSE(src.finished());

  const nxm::video::GpuFrame back = src.frame_at(0.0, false);
  REQUIRE(back.valid());
  CHECK(src.position() < 0.2);
  {
    const nxe::rhi::ReadbackResult y =
        fixture.device.uploader().read_texture(back.luma);
    REQUIRE(y.data != nullptr);
    fixture.device.uploader().wait(y.ticket);
    const VpxLuma ref = vpx_decode_luma(frames, 0);
    REQUIRE(ref.ok);
    usize mism = 0;
    for (u32 row = 0; row < 240; ++row)
      for (u32 col = 0; col < 320; ++col)
        if (y.data[row * src.width() + col] != ref.y[row * 320 + col])
          ++mism;
    CHECK(mism == 0u);
  }

  for (int i = 0; i < 5; ++i)
    (void)src.frame_at(100.0, false);
  CHECK(src.finished());
  CHECK(src.frame_at(100.0, false).valid());
  REQUIRE(src.frame_at(3.1, true).valid());
  CHECK_FALSE(src.finished());
  CHECK(src.position() >= 3.0);

  nxm::video::HwVideoSource resumed;
  REQUIRE(resumed.open(fixture.device, "/test.webm"));
  REQUIRE(resumed.seek(1.0, false));
  REQUIRE(resumed.frame_at(1.0, false).valid());
  CHECK(resumed.position() > 0.8);

  nxm::video::HwVideoSource looped;
  REQUIRE(looped.open(fixture.device, "/test.webm"));
  REQUIRE(looped.frame_at(3.1, true).valid());
  CHECK(looped.position() >= 3.0);
  CHECK(looped.position() < 3.2);
  CHECK_FALSE(looped.finished());
}

TEST_CASE("gpu source: the factory drives the CPU backend behind one interface") {
  TestDevice fixture;
  if (!fixture.ready)
    SKIP("no usable RHI device");

  REQUIRE(nx::vfs::initialize());
  struct Unmount {
    ~Unmount() { nx::vfs::unmount_all(); }
  } unmount;
  REQUIRE(nx::vfs::mount("/", nx::vfs::make_host_device(NX_VIDEO_FIXTURE_DIR), 0)
              .valid());

  const nxm::video::GpuSourcePtr src =
      nxm::video::create_video_source(fixture.device, "/test_av1.webm");
  REQUIRE(src != nullptr);
  CHECK(src->width() == 320u);
  CHECK(src->height() == 240u);
  CHECK(src->frame_rate() > 0.0);

  const nxm::video::GpuFrame first = src->frame_at(0.0, false);
  REQUIRE(first.valid());
  CHECK(src->position() < 0.2);

  const nxm::video::GpuFrame later = src->frame_at(1.0, false);
  REQUIRE(later.valid());
  CHECK(src->position() > 0.8);
  CHECK_FALSE(src->finished());

  REQUIRE(src->seek(0.5, false));
  REQUIRE(src->frame_at(0.5, false).valid());
  CHECK(src->position() > 0.3);
  CHECK(src->position() < 0.7);

  REQUIRE(src->seek(3.1, true));
  REQUIRE(src->frame_at(3.1, true).valid());
  CHECK(src->position() >= 0.0);
  CHECK(src->position() < 0.2);
  CHECK_FALSE(src->finished());

  const nxm::video::GpuFrame back = src->frame_at(0.0, false);
  REQUIRE(back.valid());
  CHECK(src->position() < 0.2);
  for (int i = 0; i < 200; ++i)
    (void)src->frame_at(100.0, false);
  CHECK(src->finished());
}

TEST_CASE("vp9 parse: a truncated or empty frame is rejected, not walked off") {
  std::vector<u8> backing;
  const std::vector<RawFrame> frames =
      read_frames(NX_VIDEO_FIXTURE_DIR "/test.webm", backing, 2);
  REQUIRE(!frames.empty());

  rhi::Vp9FrameHeader header;
  CHECK_FALSE(rhi::parse_vp9_frame_header(nullptr, 0, nullptr, nullptr, header));
  CHECK_FALSE(
      rhi::parse_vp9_frame_header(frames[0].bytes.data(), 0, nullptr, nullptr,
                                  header));

  CHECK_FALSE(rhi::parse_vp9_frame_header(frames[0].bytes.data(), 2, nullptr,
                                          nullptr, header));
}
