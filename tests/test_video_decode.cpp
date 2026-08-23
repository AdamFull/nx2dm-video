
#include "framework/nxtest.h"

#include "video/video_audio.h"
#include "video/video_clip.h"
#include "video/video_decode.h"
#include "video/video_demux.h"
#include "video/video_mediacodec.h"
#include "video/video_source.h"

#include "core/foundation/vfs/vfs.h"

#include <cmath>
#include <cstdlib>
#include <limits>

namespace {

struct MountedFixtures {
  bool ok = false;
  MountedFixtures() {
    if (!nx::vfs::initialize())
      return;
    ok = nx::vfs::mount("/", nx::vfs::make_host_device(NX_VIDEO_FIXTURE_DIR), 0)
             .valid();
  }
  ~MountedFixtures() { nx::vfs::unmount_all(); }
  MountedFixtures(const MountedFixtures &) = delete;
  MountedFixtures &operator=(const MountedFixtures &) = delete;
};

class CountingSource final : public nxm::video::FrameSource {
public:
  explicit CountingSource(const f64 fps) noexcept : m_fps(fps) {}

  [[nodiscard]] u32 width() const noexcept override { return 4; }
  [[nodiscard]] u32 height() const noexcept override { return 4; }
  [[nodiscard]] f64 frame_rate() const noexcept override { return m_fps; }

  [[nodiscard]] bool next(nxm::video::VideoFrame &out) override {
    ++decodes;
    out.width = 4;
    out.height = 4;
    out.y_pitch = 4;
    out.c_pitch = 2;
    out.pts = nx::cast<f64>(m_frame++) / m_fps;
    out.y.assign(16, 0);
    out.cb.assign(4, 128);
    out.cr.assign(4, 128);
    return true;
  }
  void restart() override { m_frame = 0; }

  int decodes = 0;

private:
  f64 m_fps;
  u64 m_frame = 0;
};

class FiniteCountingSource final : public nxm::video::FrameSource {
public:
  [[nodiscard]] u32 width() const noexcept override { return 4; }
  [[nodiscard]] u32 height() const noexcept override { return 4; }
  [[nodiscard]] f64 frame_rate() const noexcept override { return 10.0; }

  [[nodiscard]] bool next(nxm::video::VideoFrame &out) override {
    if (m_frame == 3)
      return false;
    out.width = 4;
    out.height = 4;
    out.y_pitch = 4;
    out.c_pitch = 2;
    out.pts = nx::cast<f64>(m_frame++) / 10.0;
    out.y.assign(16, 0);
    out.cb.assign(4, 128);
    out.cr.assign(4, 128);
    ++decodes;
    return true;
  }

  void restart() override {
    m_frame = 0;
    ++restarts;
  }

  int decodes = 0;
  int restarts = 0;

private:
  u32 m_frame = 0;
};

[[nodiscard]] bool same(const nx::vector<u8> &a, const nx::vector<u8> &b) {
  if (a.size() != b.size())
    return false;
  for (usize i = 0; i < a.size(); ++i)
    if (a[i] != b[i])
      return false;
  return true;
}

[[nodiscard]] u64 count_samples(nxe::audio::IDecoder &dec) {
  std::vector<i16> buf(nx::cast<usize>(48000u) * dec.format().channels);
  u64 total = 0;
  for (;;) {
    const u64 n = dec.read(buf.data(), 48000);
    if (n == 0)
      break;
    total += n;
    if (total > nx::cast<u64>(48000) * 60)
      break;
  }
  return total;
}

[[nodiscard]] int decode_all(nxm::video::FrameSource &src) {
  nxm::video::VideoFrame frame;
  int count = 0;
  while (src.next(frame)) {
    ++count;
    if (count > 1000)
      break;
  }
  return count;
}

}

TEST_CASE("video decode: a VP9 webm opens at its true size") {
  const MountedFixtures fixtures;
  REQUIRE(fixtures.ok);

  const nxm::video::SourcePtr src = nxm::video::open_webm("/test.webm");
  REQUIRE(src != nullptr);
  CHECK(src->width() == 320);
  CHECK(src->height() == 240);
  CHECK(src->frame_rate() > 0.0);
}

TEST_CASE("video decode: the codec peek tells VP9 from AV1 (hw path gate)") {
  const MountedFixtures fixtures;
  REQUIRE(fixtures.ok);
  CHECK(nxm::video::webm_is_vp9("/test.webm"));
  CHECK_FALSE(nxm::video::webm_is_vp9("/test_av1.webm"));
  CHECK_FALSE(nxm::video::webm_is_vp9("/nope.webm"));
}

TEST_CASE("video decode: an AV1 webm decodes through the same FrameSource") {
  const MountedFixtures fixtures;
  REQUIRE(fixtures.ok);

  const nxm::video::SourcePtr src = nxm::video::open_webm("/test_av1.webm");
  REQUIRE(src != nullptr);
  CHECK(src->width() == 320);
  CHECK(src->height() == 240);
  CHECK(src->frame_rate() > 0.0);

  nxm::video::VideoFrame first;
  REQUIRE(src->next(first));
  CHECK(first.width == 320);
  CHECK(first.height == 240);
  CHECK(first.y.size() == 320u * 240u);
  CHECK(first.cb.size() == 160u * 120u);
  bool varies = false;
  for (usize i = 1; i < first.y.size() && !varies; ++i)
    varies = first.y[i] != first.y[0];
  CHECK(varies);

  const int count = 1 + decode_all(*src);
  CHECK(count == 45);
  src->restart();
  CHECK(decode_all(*src) == 45);
}

TEST_CASE("video demux: re-opens for a second codec id on the same object") {
  const MountedFixtures fixtures;
  REQUIRE(fixtures.ok);

  nxm::video::WebmVideoDemux demux;
  CHECK_FALSE(demux.open("/test.webm", "V_VP8"));
  REQUIRE(demux.open("/test.webm", "V_VP9"));
  CHECK(demux.width() == 320);
  CHECK(demux.height() == 240);

  const u8 *data = nullptr;
  long len = 0;
  f64 pts = 0.0;
  REQUIRE(demux.next(data, len, pts));
  CHECK(len > 0);
  CHECK(demux.max_frame_size() >= nx::cast<usize>(len));
}

TEST_CASE("MediaCodec planes: padded and interleaved input copies safely") {
  const u8 src[] = {1, 90, 2, 91, 3, 92, 4, 93, 5, 94, 6, 95};
  u8 dst[6] = {};
  REQUIRE(nxm::video::mediacodec_detail::copy_plane_checked(
      dst, sizeof(dst), 3, src, sizeof(src), 6, 2, 3, 2));
  for (usize i = 0; i < 6; ++i)
    CHECK(dst[i] == i + 1);
}

TEST_CASE("MediaCodec planes: inconsistent vendor bounds are rejected") {
  const u8 src[12] = {};
  u8 dst[6] = {};
  CHECK_FALSE(nxm::video::mediacodec_detail::copy_plane_checked(
      dst, sizeof(dst), 3, src, 10, 6, 2, 3, 2));
  CHECK_FALSE(nxm::video::mediacodec_detail::copy_plane_checked(
      dst, sizeof(dst) - 1, 3, src, sizeof(src), 6, 2, 3, 2));
  CHECK_FALSE(nxm::video::mediacodec_detail::copy_plane_checked(
      dst, sizeof(dst), 3, src, sizeof(src), 0, 2, 3, 2));
  CHECK_FALSE(nxm::video::mediacodec_detail::copy_plane_checked(
      dst, sizeof(dst), 3, src, sizeof(src), 6, 0, 3, 2));
}

TEST_CASE("video frame: chroma interleaves into one NV12 R8G8 plane") {
  nxm::video::VideoFrame f;
  f.width = 4;
  f.height = 4;
  f.cb = {10, 11, 12, 13};
  f.cr = {20, 21, 22, 23};

  nx::vector<u8> out;
  nxm::video::interleave_chroma(f, out);
  REQUIRE(out.size() == 8);
  CHECK(out[0] == 10);
  CHECK(out[1] == 20);
  CHECK(out[2] == 11);
  CHECK(out[3] == 21);
  CHECK(out[6] == 13);
  CHECK(out[7] == 23);

  nxm::video::VideoFrame bad;
  bad.width = 4;
  bad.height = 4;
  bad.cb = {1, 2};
  nx::vector<u8> empty;
  nxm::video::interleave_chroma(bad, empty);
  CHECK(empty.empty());
}

TEST_CASE("video decode: every frame of the clip comes out, twice") {
  const MountedFixtures fixtures;
  REQUIRE(fixtures.ok);

  const nxm::video::SourcePtr src = nxm::video::open_webm("/test.webm");
  REQUIRE(src != nullptr);

  nxm::video::VideoFrame first;
  REQUIRE(src->next(first));
  CHECK(first.width == 320);
  CHECK(first.height == 240);
  CHECK(first.y.size() == 320u * 240u);
  CHECK(first.cb.size() == 160u * 120u);

  int count = 1 + decode_all(*src);
  CHECK(count == 45);

  src->restart();
  CHECK(decode_all(*src) == 45);
}

TEST_CASE("video decode: colour matrix and range come from the container") {
  const MountedFixtures fixtures;
  REQUIRE(fixtures.ok);

  const nxm::video::SourcePtr tagged =
      nxm::video::open_webm("/test_bt601_full.webm");
  REQUIRE(tagged != nullptr);
  nxm::video::VideoFrame frame;
  REQUIRE(tagged->next(frame));
  CHECK(frame.colour.matrix == nxm::video::ColourMatrix::BT601);
  CHECK(frame.colour.full_range);

  const nxm::video::SourcePtr untagged = nxm::video::open_webm("/test.webm");
  REQUIRE(untagged != nullptr);
  nxm::video::VideoFrame sd;
  REQUIRE(untagged->next(sd));
  CHECK(sd.colour.matrix == nxm::video::ColourMatrix::BT601);
  CHECK_FALSE(sd.colour.full_range);
}

TEST_CASE("video clip: a .nxvid resolves to its source and loop") {
  const MountedFixtures fixtures;
  REQUIRE(fixtures.ok);

  nxm::video::VideoClip clip;
  REQUIRE(nxm::video::load_video_clip("/test_clip.nxvid", clip));
  CHECK(clip.source == "/test.webm");
  CHECK_FALSE(clip.loop);

  nxm::video::VideoClip missing;
  CHECK_FALSE(nxm::video::load_video_clip("/nope.nxvid", missing));
}

TEST_CASE("video decode: a missing clip is a null source, not a crash") {
  const MountedFixtures fixtures;
  REQUIRE(fixtures.ok);
  CHECK(nxm::video::open_webm("/nope.webm") == nullptr);
}

TEST_CASE("video pacing: the frame shown is the one the clock has reached") {
  nxm::video::PacedPlayback pacer;
  pacer.reset(std::make_unique<nxm::video::SyntheticSource>(16, 16, 10.0),
              false);

  const nxm::video::VideoFrame *f = pacer.advance(0.0);
  REQUIRE(f != nullptr);
  CHECK(f->pts < 0.001);

  f = pacer.advance(0.05);
  REQUIRE(f != nullptr);
  CHECK(f->pts < 0.001);

  f = pacer.advance(0.05);
  REQUIRE(f != nullptr);
  CHECK(f->pts > 0.09);
  CHECK(f->pts < 0.11);

  f = pacer.advance(0.25);
  REQUIRE(f != nullptr);
  CHECK(f->pts > 0.29);
  CHECK(f->pts < 0.31);

  const nxm::video::VideoFrame *g = pacer.advance(0.0);
  REQUIRE(g != nullptr);
  CHECK(g->pts > 0.29);
  CHECK(g->pts < 0.31);
}

TEST_CASE("video pacing: absolute targets stay monotonic across a loop") {
  auto src = std::make_unique<FiniteCountingSource>();
  FiniteCountingSource *const raw = src.get();
  nxm::video::PacedPlayback pacer;
  pacer.reset(std::move(src), true);

  REQUIRE(pacer.advance_to(0.0) != nullptr);
  REQUIRE(pacer.advance_to(0.31) != nullptr);
  REQUIRE(raw->restarts == 1);
  const int after_wrap = raw->decodes;

  const nxm::video::VideoFrame *const next = pacer.advance_to(0.32);
  REQUIRE(next != nullptr);
  CHECK(next->pts < 0.1);
  CHECK(raw->restarts == 1);
  CHECK(raw->decodes == after_wrap);
}

TEST_CASE("video pacing: a loop preserves a large target's cycle position") {
  auto src = std::make_unique<FiniteCountingSource>();
  FiniteCountingSource *const raw = src.get();
  nxm::video::PacedPlayback pacer;
  pacer.reset(std::move(src), true);

  const nxm::video::VideoFrame *const frame = pacer.advance_to(0.55);
  REQUIRE(frame != nullptr);
  CHECK(std::fabs(frame->pts - 0.2) < 1e-9);
  CHECK(raw->restarts == 1);
}

TEST_CASE("video pacing: a backward target restarts a source without seek") {
  auto src = std::make_unique<FiniteCountingSource>();
  FiniteCountingSource *const raw = src.get();
  nxm::video::PacedPlayback pacer;
  pacer.reset(std::move(src), false);

  const nxm::video::VideoFrame *frame = pacer.advance_to(0.2);
  REQUIRE(frame != nullptr);
  CHECK(std::fabs(frame->pts - 0.2) < 1e-9);

  frame = pacer.advance_to(0.05);
  REQUIRE(frame != nullptr);
  CHECK(frame->pts < 0.1);
  CHECK(raw->restarts == 1);
}

TEST_CASE("video demux: memory reads reject overflowing ranges") {
  const u8 bytes[] = {1u, 2u, 3u, 4u};
  nxm::video::MemoryReader reader(bytes, 4);
  u8 out = 0;
  CHECK(reader.Read(std::numeric_limits<long long>::max() - 1, 8, &out) == -1);
  CHECK(reader.Read(3, 2, &out) == -1);
  CHECK(reader.Read(3, 1, &out) == 0);
  CHECK(out == 4u);
}

TEST_CASE("video budget: a decode cap holds decodes and catches up over calls") {
  auto src = std::make_unique<CountingSource>(60.0);
  CountingSource *const raw = src.get();
  nxm::video::PacedPlayback pacer;
  pacer.reset(std::move(src), false);

  REQUIRE(pacer.advance(0.0, nullptr) != nullptr);
  const int primed = raw->decodes;

  i32 budget = 3;
  const nxm::video::VideoFrame *f = pacer.advance(1.0, &budget);
  REQUIRE(f != nullptr);
  CHECK(raw->decodes - primed == 3);
  CHECK(budget == 0);
  CHECK(f->pts < 0.1);

  const f64 before = f->pts;
  i32 more = 3;
  f = pacer.advance(0.0, &more);
  REQUIRE(f != nullptr);
  CHECK(f->pts > before);
}

TEST_CASE("video budget: no cap catches up to the clock in one call") {
  auto src = std::make_unique<CountingSource>(60.0);
  CountingSource *const raw = src.get();
  nxm::video::PacedPlayback pacer;
  pacer.reset(std::move(src), false);
  REQUIRE(pacer.advance(0.0, nullptr) != nullptr);

  const nxm::video::VideoFrame *f = pacer.advance(1.0, nullptr);
  REQUIRE(f != nullptr);
  CHECK(f->pts > 0.9);
  CHECK(raw->decodes > 50);
}

TEST_CASE("video audio: the Opus track decodes to 48 kHz PCM, and has sound") {
  const MountedFixtures fixtures;
  REQUIRE(fixtures.ok);

  nxe::audio::DecoderPtr audio =
      nxm::video::open_webm_opus("/test_audio.webm");
  REQUIRE(audio != nullptr);
  CHECK(audio->format().sample_rate == 48000u);
  CHECK(audio->format().channels == 1u);

  const u32 ch = audio->format().channels;
  std::vector<i16> buf(nx::cast<usize>(48000u) * ch);

  const u64 first = audio->read(buf.data(), 48000);
  CHECK(first > 0);
  i16 peak = 0;
  for (u64 i = 0; i < first * ch; ++i)
    peak = nx::max<i16>(peak, nx::cast<i16>(std::abs(nx::cast<int>(buf[i]))));
  CHECK(peak > 1000);

  u64 total = first;
  for (;;) {
    const u64 n = audio->read(buf.data(), 48000);
    if (n == 0)
      break;
    total += n;
    if (total > nx::cast<u64>(48000) * 10)
      break;
  }
  CHECK(total > 120000);
  CHECK(total < 180000);

  CHECK(nxm::video::open_webm_opus("/test.webm") == nullptr);
  CHECK(nxm::video::open_webm_opus("/nope.webm") == nullptr);
}

TEST_CASE("video audio: a Vorbis track decodes to PCM, and has sound") {
  const MountedFixtures fixtures;
  REQUIRE(fixtures.ok);

  const nxe::audio::DecoderPtr audio =
      nxm::video::open_webm_vorbis("/test_vorbis.webm");
  REQUIRE(audio != nullptr);
  CHECK(audio->format().sample_rate == 48000u);
  CHECK(audio->format().channels == 1u);

  const u32 ch = audio->format().channels;
  std::vector<i16> buf(nx::cast<usize>(48000u) * ch);

  const u64 first = audio->read(buf.data(), 48000);
  CHECK(first > 0);
  i16 peak = 0;
  for (u64 i = 0; i < first * ch; ++i)
    peak = nx::max<i16>(peak, nx::cast<i16>(std::abs(nx::cast<int>(buf[i]))));
  CHECK(peak > 1000);

  u64 total = first;
  for (;;) {
    const u64 n = audio->read(buf.data(), 48000);
    if (n == 0)
      break;
    total += n;
    if (total > nx::cast<u64>(48000) * 10)
      break;
  }
  CHECK(total > 100000);
  CHECK(total < 140000);

  CHECK(nxm::video::open_webm_audio("/test_vorbis.webm") != nullptr);
  CHECK(nxm::video::open_webm_audio("/test_audio.webm") != nullptr);
  CHECK(nxm::video::open_webm_vorbis("/test_audio.webm") == nullptr);
  CHECK(nxm::video::open_webm_vorbis("/nope.webm") == nullptr);
}

TEST_CASE("video audio: an audio track seeks to a time, and home again") {
  const MountedFixtures fixtures;
  REQUIRE(fixtures.ok);

  struct Clip {
    nx::string_view path;
    nxe::audio::DecoderPtr (*open)(nx::string_view);
  };
  const Clip clips[] = {{"/test_audio.webm", &nxm::video::open_webm_opus},
                        {"/test_vorbis.webm", &nxm::video::open_webm_vorbis}};

  for (const Clip &clip : clips) {
    const nxe::audio::DecoderPtr full = clip.open(clip.path);
    REQUIRE(full != nullptr);
    const u64 rate = full->format().sample_rate;
    const u64 total = count_samples(*full);
    REQUIRE(total > rate);

    const nxe::audio::DecoderPtr dec = clip.open(clip.path);
    REQUIRE(dec != nullptr);
    const u64 target = total / 2;
    REQUIRE(dec->seek(target));

    const u64 remaining = count_samples(*dec);
    const u64 expected = total - target;
    CHECK(remaining + rate / 10 > expected);
    CHECK(remaining < expected + rate / 10);

    REQUIRE(dec->seek(0));
    CHECK(count_samples(*dec) + rate / 10 > total);
  }
}

TEST_CASE("video pacing: a looping clip never finishes; a plain one does") {
  const MountedFixtures fixtures;
  REQUIRE(fixtures.ok);

  nxm::video::PacedPlayback looping;
  looping.reset(nxm::video::open_webm("/test.webm"), true);
  REQUIRE(looping.advance(0.0) != nullptr);
  for (int i = 0; i < 200; ++i)
    (void)looping.advance(0.05);
  CHECK(!looping.finished());
  CHECK(looping.advance(0.05) != nullptr);

  nxm::video::PacedPlayback once;
  once.reset(nxm::video::open_webm("/test.webm"), false);
  const nxm::video::VideoFrame *last = nullptr;
  for (int i = 0; i < 200; ++i)
    if (const nxm::video::VideoFrame *f = once.advance(0.05))
      last = f;
  CHECK(once.finished());
  CHECK(last != nullptr);
}

TEST_CASE("video seek: keyframe-accurate, and matches linear playback") {
  const MountedFixtures fixtures;
  REQUIRE(fixtures.ok);

  constexpr usize K = 20;
  nx::vector<f64> pts;
  nxm::video::VideoFrame reference;
  {
    const nxm::video::SourcePtr src = nxm::video::open_webm("/test.webm");
    REQUIRE(src != nullptr);
    nxm::video::VideoFrame frame;
    usize i = 0;
    while (src->next(frame)) {
      pts.push_back(frame.pts);
      if (i == K)
        reference = frame;
      ++i;
    }
  }
  REQUIRE(pts.size() > K + 2);

  nxm::video::PacedPlayback pacer;
  pacer.reset(nxm::video::open_webm("/test.webm"), false);
  REQUIRE(pacer.advance(0.0) != nullptr);

  REQUIRE(pacer.seek(pts[K]));
  const nxm::video::VideoFrame *got = pacer.advance(0.0);
  REQUIRE(got != nullptr);
  CHECK(std::fabs(got->pts - pts[K]) < 1e-6);
  REQUIRE(got->width == reference.width);
  REQUIRE(got->height == reference.height);
  CHECK(same(got->y, reference.y));
  CHECK(same(got->cb, reference.cb));
  CHECK(same(got->cr, reference.cr));

  REQUIRE(pacer.seek(0.0));
  got = pacer.advance(0.0);
  REQUIRE(got != nullptr);
  CHECK(got->pts < pts[1]);

  REQUIRE(pacer.seek(pts.back() + 10.0));
  got = pacer.advance(0.0);
  REQUIRE(got != nullptr);
  CHECK(got->pts >= pts.back() - 1e-6);
}
