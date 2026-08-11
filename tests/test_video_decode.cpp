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
#include "video/video_decode.h"
#include "video/video_source.h"

#include "core/foundation/vfs/vfs.h"

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
