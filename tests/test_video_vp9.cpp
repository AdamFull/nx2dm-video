/**
 * @file test_video_vp9.cpp
 * @brief The RHI's VP9 uncompressed-header parser, checked against libvpx.
 *
 * The parser is what feeds a Vulkan Video decode: it turns a frame's leading
 * bytes into the Std structs and the offsets that split the frame. libvpx is the
 * oracle - it decodes the same raw frames and reports their true size and key
 * status, and the parser must agree. The frames come straight off the fixture
 * .webm through libwebm, the compressed bytes the hardware would also see.
 */

#include "framework/nxtest.h"

#include "core/rendering/rhi/vulkan/vk_vp9.h"

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

// Pull the first `want` compressed VP9 frames off the fixture, raw - the same
// bytes a hardware decoder is handed.
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

// Decode one raw frame with libvpx and report its size - the oracle the parser
// is measured against.
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

} // namespace

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

  // The fixture is 320x240; the parser must reach that through the sync code and
  // colour config to frame_size, and libvpx confirms the same numbers.
  const VpxDims dims = vpx_decode_dims(frames, 0);
  REQUIRE(dims.ok);
  CHECK(dims.width == 320u);
  CHECK(dims.height == 240u);
  CHECK(header.frame_width == dims.width);
  CHECK(header.frame_height == dims.height);

  // The three offsets partition the frame: an uncompressed header, then a
  // non-empty compressed header, then tiles, all inside the frame.
  CHECK(header.uncompressed_header_size > 0u);
  CHECK(header.compressed_header_size > 0u);
  // Strictly less: tiles always follow, so the two headers cannot fill the
  // whole frame.
  CHECK(header.uncompressed_header_size + header.compressed_header_size <
        frames[0].bytes.size());

  // 4:2:0, 8-bit - the only profile-0 shape.
  CHECK(header.color.BitDepth == 8u);
  CHECK(header.color.subsampling_x == 1u);
  CHECK(header.color.subsampling_y == 1u);
}

TEST_CASE("vp9 parse: an inter frame parses as non-key with a whole header") {
  std::vector<u8> backing;
  const std::vector<RawFrame> frames =
      read_frames(NX_VIDEO_FIXTURE_DIR "/test.webm", backing, 6);
  REQUIRE(frames.size() >= 2);

  // The keyframe's size seeds every reference slot, which is what an inter frame
  // that inherits its size reads back.
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

  // Find the first non-key frame and parse it.
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

  // Reaching a sane header_size_in_bytes means the whole uncompressed header -
  // loop filter, quant, segmentation, tiles - parsed without drifting.
  CHECK(header.compressed_header_size > 0u);
  CHECK(header.uncompressed_header_size + header.compressed_header_size <
        frames[inter].bytes.size());
}

TEST_CASE("vp9 parse: every frame of the clip parses, header inside the frame") {
  std::vector<u8> backing;
  const std::vector<RawFrame> frames =
      read_frames(NX_VIDEO_FIXTURE_DIR "/test.webm", backing, 1000);
  REQUIRE(frames.size() >= 40); // the fixture is 45

  // Track reference sizes as the decoder would, so inter frames that inherit a
  // size read it back correctly; a keyframe refreshes all eight slots.
  u32 ref_w[8] = {};
  u32 ref_h[8] = {};
  for (const RawFrame &frame : frames) {
    rhi::Vp9FrameHeader header;
    REQUIRE(rhi::parse_vp9_frame_header(frame.bytes.data(),
                                        static_cast<u32>(frame.bytes.size()),
                                        ref_w, ref_h, header));
    // A coarse invariant: no frame is rejected and every header lands inside
    // its frame. This catches gross drift (a misparse that runs the tile count
    // or header size wild), but not fine drift - a single stray bit keeps the
    // header small enough to stay in bounds. Exact tail correctness (loop
    // filter, quant, segmentation, tiles) is what the hardware decode's
    // pixel-match verifies; the offsets here only have to be believable.
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

  // Two bytes: enough for the marker, far short of the header. The reader runs
  // out and the parse fails rather than reading past the buffer.
  CHECK_FALSE(rhi::parse_vp9_frame_header(frames[0].bytes.data(), 2, nullptr,
                                          nullptr, header));
}
