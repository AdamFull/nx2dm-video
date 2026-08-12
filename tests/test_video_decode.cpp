/**
 * @file test_video_decode.cpp
 * @brief That a real .webm demuxes and decodes: the right size, the right frame
 * count, and that it can be played twice.
 *
 * The fixture is a 320x240 VP9 clip, 45 frames, made with ffmpeg (see the
 * module's tests/fixtures). These cases read it through the VFS exactly as the
 * runtime does.
 */

#include "framework/nxtest.h"

#include "video/video_audio.h"
#include "video/video_clip.h"
#include "video/video_decode.h"
#include "video/video_demux.h"
#include "video/video_source.h"

#include "core/foundation/vfs/vfs.h"

#include <cmath>
#include <cstdlib>

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

/// A source that counts how many frames were pulled from it, so a budget cap
/// can be measured rather than inferred. Endless, so it never ends on its own.
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
    if (total > nx::cast<u64>(48000) * 60) // runaway guard
      break;
  }
  return total;
}

[[nodiscard]] int decode_all(nxm::video::FrameSource &src) {
  nxm::video::VideoFrame frame;
  int count = 0;
  while (src.next(frame)) {
    ++count;
    if (count > 1000) // a runaway guard; the fixture is 45
      break;
  }
  return count;
}

} // namespace

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
  // The module uses this to keep AV1 off the VP9-only hardware path.
  CHECK(nxm::video::webm_is_vp9("/test.webm"));
  CHECK_FALSE(nxm::video::webm_is_vp9("/test_av1.webm"));
  CHECK_FALSE(nxm::video::webm_is_vp9("/nope.webm"));
}

TEST_CASE("video decode: an AV1 webm decodes through the same FrameSource") {
  const MountedFixtures fixtures;
  REQUIRE(fixtures.ok);

  // open_webm dispatches on the track codec: this file is AV1, so it comes back
  // a libgav1-backed source, indistinguishable to the caller from the VP9 one.
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
  // The test pattern is strongly coloured, so a real decode is not flat grey.
  bool varies = false;
  for (usize i = 1; i < first.y.size() && !varies; ++i)
    varies = first.y[i] != first.y[0];
  CHECK(varies);

  // The clip is 45 frames and, like the VP9 one, plays again from the top.
  const int count = 1 + decode_all(*src);
  CHECK(count == 45);
  src->restart();
  CHECK(decode_all(*src) == 45);
}

TEST_CASE("video demux: re-opens for a second codec id on the same object") {
  const MountedFixtures fixtures;
  REQUIRE(fixtures.ok);

  // The MediaCodec source tries VP9 then AV1 on one demux; the second open has
  // to discard the first attempt's segment and read the track cleanly. Nothing
  // else re-opens a demux, so this is where that contract is pinned.
  nxm::video::WebmVideoDemux demux;
  CHECK_FALSE(demux.open("/test.webm", "V_VP8")); // right file, absent codec
  REQUIRE(demux.open("/test.webm", "V_VP9"));      // re-open finds the real track
  CHECK(demux.width() == 320);
  CHECK(demux.height() == 240);

  const u8 *data = nullptr;
  long len = 0;
  f64 pts = 0.0;
  REQUIRE(demux.next(data, len, pts)); // and yields real packets after re-open
  CHECK(len > 0);
}

TEST_CASE("video decode: every frame of the clip comes out, twice") {
  const MountedFixtures fixtures;
  REQUIRE(fixtures.ok);

  const nxm::video::SourcePtr src = nxm::video::open_webm("/test.webm");
  REQUIRE(src != nullptr);

  // First frame carries a full-size I420 luma plane and neutral-ish content.
  nxm::video::VideoFrame first;
  REQUIRE(src->next(first));
  CHECK(first.width == 320);
  CHECK(first.height == 240);
  CHECK(first.y.size() == 320u * 240u);
  CHECK(first.cb.size() == 160u * 120u);

  int count = 1 + decode_all(*src);
  CHECK(count == 45);

  // Playable again from the top: the decoder resets to the opening keyframe.
  src->restart();
  CHECK(decode_all(*src) == 45);
}

TEST_CASE("video decode: colour matrix and range come from the container") {
  const MountedFixtures fixtures;
  REQUIRE(fixtures.ok);

  // test_bt601_full.webm is tagged BT.601 (smpte170m) full-range: the frame must
  // carry that, not the BT.709-limited default the code assumed before.
  const nxm::video::SourcePtr tagged =
      nxm::video::open_webm("/test_bt601_full.webm");
  REQUIRE(tagged != nullptr);
  nxm::video::VideoFrame frame;
  REQUIRE(tagged->next(frame));
  CHECK(frame.colour.matrix == nxm::video::ColourMatrix::BT601);
  CHECK(frame.colour.full_range);

  // The untagged 320x240 fixture leaves the matrix unspecified, so the
  // resolution heuristic decides: SD is BT.601, and its range is broadcast.
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

  // test_clip.nxvid names /test.webm and turns looping off; both must come back
  // from the descriptor, not the struct defaults (loop defaults to true).
  nxm::video::VideoClip clip;
  REQUIRE(nxm::video::load_video_clip("/test_clip.nxvid", clip));
  CHECK(clip.source == "/test.webm");
  CHECK_FALSE(clip.loop);

  // A descriptor that will not read is a clean false, not a crash.
  nxm::video::VideoClip missing;
  CHECK_FALSE(nxm::video::load_video_clip("/nope.nxvid", missing));
}

TEST_CASE("video decode: a missing clip is a null source, not a crash") {
  const MountedFixtures fixtures;
  REQUIRE(fixtures.ok);
  CHECK(nxm::video::open_webm("/nope.webm") == nullptr);
}

TEST_CASE("video pacing: the frame shown is the one the clock has reached") {
  // A source at 10 fps: frame k is due at k/10 s. The pacer must show the
  // newest frame whose PTS the clock has passed, and no newer.
  nxm::video::PacedPlayback pacer;
  pacer.reset(std::make_unique<nxm::video::SyntheticSource>(16, 16, 10.0),
              false);

  const nxm::video::VideoFrame *f = pacer.advance(0.0);
  REQUIRE(f != nullptr);
  CHECK(f->pts < 0.001); // frame 0

  f = pacer.advance(0.05); // clock 0.05: still frame 0
  REQUIRE(f != nullptr);
  CHECK(f->pts < 0.001);

  f = pacer.advance(0.05); // clock 0.10: frame 1
  REQUIRE(f != nullptr);
  CHECK(f->pts > 0.09);
  CHECK(f->pts < 0.11);

  f = pacer.advance(0.25); // clock 0.35: frame 3, having dropped 2
  REQUIRE(f != nullptr);
  CHECK(f->pts > 0.29);
  CHECK(f->pts < 0.31);

  // A held clock shows the same frame, not the next.
  const nxm::video::VideoFrame *g = pacer.advance(0.0);
  REQUIRE(g != nullptr);
  CHECK(g->pts > 0.29);
  CHECK(g->pts < 0.31);
}

TEST_CASE("video budget: a decode cap holds decodes and catches up over calls") {
  auto src = std::make_unique<CountingSource>(60.0);
  CountingSource *const raw = src.get();
  nxm::video::PacedPlayback pacer;
  pacer.reset(std::move(src), false);

  // Prime uncapped: the first frame and its successor, two decodes.
  REQUIRE(pacer.advance(0.0, nullptr) != nullptr);
  const int primed = raw->decodes;

  // One second is 60 frames due; a budget of 3 decodes at most three of them.
  i32 budget = 3;
  const nxm::video::VideoFrame *f = pacer.advance(1.0, &budget);
  REQUIRE(f != nullptr);
  CHECK(raw->decodes - primed == 3);
  CHECK(budget == 0);
  CHECK(f->pts < 0.1); // a few frames in, nowhere near the whole second

  // The clock is already a second in; a further budgeted call keeps catching up
  // rather than stalling, which is what keeps a starved clip moving.
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
  CHECK(f->pts > 0.9); // reached the frame a full second in
  CHECK(raw->decodes > 50);
}

TEST_CASE("video audio: the Opus track decodes to 48 kHz PCM, and has sound") {
  const MountedFixtures fixtures;
  REQUIRE(fixtures.ok);

  // test_audio.webm is the video fixture plus a 440 Hz sine on an Opus track.
  nxe::audio::DecoderPtr audio =
      nxm::video::open_webm_opus("/test_audio.webm");
  REQUIRE(audio != nullptr);
  CHECK(audio->format().sample_rate == 48000u);
  CHECK(audio->format().channels == 1u);

  const u32 ch = audio->format().channels;
  std::vector<i16> buf(nx::cast<usize>(48000u) * ch);

  const u64 first = audio->read(buf.data(), 48000);
  CHECK(first > 0);
  // A sine, not silence: something well clear of the noise floor comes out.
  i16 peak = 0;
  for (u64 i = 0; i < first * ch; ++i)
    peak = nx::max<i16>(peak, nx::cast<i16>(std::abs(nx::cast<int>(buf[i]))));
  CHECK(peak > 1000);

  // The whole ~3 s clip decodes and then ends.
  u64 total = first;
  for (;;) {
    const u64 n = audio->read(buf.data(), 48000);
    if (n == 0)
      break;
    total += n;
    if (total > nx::cast<u64>(48000) * 10) // runaway guard
      break;
  }
  CHECK(total > 120000); // at least ~2.5 s of the 3 s clip
  CHECK(total < 180000); // and not much past 3 s

  // A missing track is a null decoder, and the video fixture has no audio.
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
  CHECK(peak > 1000); // a 440 Hz sine, not silence

  u64 total = first;
  for (;;) {
    const u64 n = audio->read(buf.data(), 48000);
    if (n == 0)
      break;
    total += n;
    if (total > nx::cast<u64>(48000) * 10)
      break;
  }
  CHECK(total > 100000); // ~2.5 s at 48 kHz
  CHECK(total < 140000);

  // The codec-agnostic opener picks whichever track the file carries, and each
  // codec's opener declines the other's file.
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
    REQUIRE(total > rate); // at least a second to seek within

    const nxe::audio::DecoderPtr dec = clip.open(clip.path);
    REQUIRE(dec != nullptr);
    const u64 target = total / 2;
    REQUIRE(dec->seek(target));

    // Landed near the target: the tail is about (total - target). The tolerance
    // is a tenth of a second, which covers block alignment and the reset's
    // convergence region.
    const u64 remaining = count_samples(*dec);
    const u64 expected = total - target;
    CHECK(remaining + rate / 10 > expected);
    CHECK(remaining < expected + rate / 10);

    // Seeking home restores the whole track.
    REQUIRE(dec->seek(0));
    CHECK(count_samples(*dec) + rate / 10 > total);
  }
}

TEST_CASE("video pacing: a looping clip never finishes; a plain one does") {
  const MountedFixtures fixtures;
  REQUIRE(fixtures.ok);

  // Looping: run well past the ~3 s clip and it is still producing frames.
  nxm::video::PacedPlayback looping;
  looping.reset(nxm::video::open_webm("/test.webm"), true);
  REQUIRE(looping.advance(0.0) != nullptr);
  for (int i = 0; i < 200; ++i) // 10 s
    looping.advance(0.05);
  CHECK(!looping.finished());
  CHECK(looping.advance(0.05) != nullptr);

  // Not looping: it finishes and holds the last frame.
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

  // Play the clip through once to learn its timeline and keep one frame to
  // compare a sought copy of it against, pixel for pixel.
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

  // A forward seek lands on the frame at that time - and decodes it correctly,
  // which only a reset to the right keyframe gives: the pixels must match the
  // ones the linear pass produced for the same frame.
  REQUIRE(pacer.seek(pts[K]));
  const nxm::video::VideoFrame *got = pacer.advance(0.0);
  REQUIRE(got != nullptr);
  CHECK(std::fabs(got->pts - pts[K]) < 1e-6);
  REQUIRE(got->width == reference.width);
  REQUIRE(got->height == reference.height);
  CHECK(same(got->y, reference.y));
  CHECK(same(got->cb, reference.cb));
  CHECK(same(got->cr, reference.cr));

  // Seeking home returns the first frame.
  REQUIRE(pacer.seek(0.0));
  got = pacer.advance(0.0);
  REQUIRE(got != nullptr);
  CHECK(got->pts < pts[1]);

  // Seeking past the end holds the last frame rather than failing.
  REQUIRE(pacer.seek(pts.back() + 10.0));
  got = pacer.advance(0.0);
  REQUIRE(got != nullptr);
  CHECK(got->pts >= pts.back() - 1e-6);
}
