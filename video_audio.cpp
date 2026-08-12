#include "video/video_audio.h"

#include "core/foundation/containers/blob.h"
#include "core/foundation/diagnostics/log.h"
#include "core/foundation/vfs/vfs.h"

#include "mkvparser/mkvparser.h"

#include <opus.h>

#include <cstring>

namespace nxm::video {
namespace {

/// mkvparser over the in-memory file, so it works inside an APK. Same shape as
/// the video side's reader; kept local rather than shared because it is fifteen
/// lines and sharing it would couple the two decoders' translation units.
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

constexpr i32 OPUS_RATE = 48000;
constexpr u32 MAX_CHANNELS = 2;
// The most a single Opus packet can decode to, per channel: 120 ms at 48 kHz.
constexpr i32 OPUS_MAX_FRAME = 5760;

class WebmOpusDecoder final : public nxe::audio::IDecoder {
public:
  ~WebmOpusDecoder() override {
    if (m_opus != nullptr)
      opus_decoder_destroy(m_opus);
    delete m_segment;
  }

  [[nodiscard]] bool open(nx::string_view path) {
    auto bytes = nx::vfs::read(nx::vfs::path_view(path));
    if (!bytes)
      return false;
    m_bytes = std::move(*bytes);
    m_reader = MemoryReader(m_bytes.data(), nx::cast<long long>(m_bytes.size()));

    long long pos = 0;
    if (mkvparser::EBMLHeader{}.Parse(&m_reader, pos) < 0)
      return false;
    if (mkvparser::Segment::CreateInstance(&m_reader, pos, m_segment) < 0 ||
        m_segment == nullptr || m_segment->Load() < 0)
      return false;

    const mkvparser::Tracks *const tracks = m_segment->GetTracks();
    const mkvparser::AudioTrack *audio = nullptr;
    for (unsigned long i = 0; tracks != nullptr && i < tracks->GetTracksCount();
         ++i) {
      const mkvparser::Track *const track = tracks->GetTrackByIndex(i);
      if (track != nullptr && track->GetType() == mkvparser::Track::kAudio &&
          track->GetCodecId() != nullptr &&
          std::strcmp(track->GetCodecId(), "A_OPUS") == 0) {
        audio = static_cast<const mkvparser::AudioTrack *>(track);
        break;
      }
    }
    if (audio == nullptr)
      return false;

    const long long channels = audio->GetChannels();
    if (channels < 1 || nx::cast<u32>(channels) > MAX_CHANNELS) {
      nx::logw("video: Opus track has {} channels; the mixer takes at most {}",
               channels, MAX_CHANNELS);
      return false;
    }
    m_track_number = audio->GetNumber();
    m_format.channels = nx::cast<u32>(channels);
    m_format.sample_rate = nx::cast<u32>(OPUS_RATE);
    const long long duration = m_segment->GetDuration();
    m_format.frames =
        duration > 0
            ? nx::cast<u64>(nx::cast<f64>(duration) * OPUS_RATE / 1e9)
            : 0;

    // OpusHead's pre-skip (bytes 10-11, little-endian): priming samples the
    // encoder added that must be dropped from the front, or the whole track
    // plays that many samples early. opusfile does this for the file path; here
    // it is ours to do.
    size_t private_size = 0;
    const unsigned char *const priv = audio->GetCodecPrivate(private_size);
    if (priv != nullptr && private_size >= 12)
      m_pre_skip = nx::cast<u32>(priv[10]) | (nx::cast<u32>(priv[11]) << 8);
    m_pre_skip_remaining = m_pre_skip;

    int error = 0;
    m_opus = opus_decoder_create(OPUS_RATE, nx::cast<int>(m_format.channels),
                                 &error);
    if (m_opus == nullptr || error != OPUS_OK) {
      nx::logw("video: cannot init Opus decoder ({})", error);
      return false;
    }
    // Off the stack: this runs on a scheduler worker whose stack is not the
    // main thread's, and a 20 KB decode buffer per call is how it overflows.
    m_pcm.resize(nx::cast<usize>(OPUS_MAX_FRAME) * MAX_CHANNELS);
    m_cluster = m_segment->GetFirst();
    return true;
  }

  [[nodiscard]] const nxe::audio::SoundFormat &format() const noexcept override {
    return m_format;
  }

  u64 read(i16 *out, const u64 frames) noexcept override {
    if (m_opus == nullptr || frames == 0)
      return 0;
    const u32 ch = m_format.channels;
    u64 produced = 0;
    while (produced < frames) {
      if (m_leftover_pos < m_leftover.size()) {
        const u64 have = (m_leftover.size() - m_leftover_pos) / ch;
        const u64 take = nx::min<u64>(have, frames - produced);
        std::memcpy(out + produced * ch, m_leftover.data() + m_leftover_pos,
                    nx::cast<usize>(take * ch) * sizeof(i16));
        m_leftover_pos += take * ch;
        produced += take;
        continue;
      }
      if (!decode_next())
        break; // end of stream
    }
    return produced;
  }

  [[nodiscard]] bool seek(const u64 frame) noexcept override {
    // Only a rewind to the start, which is what a loop needs; a scrub to an
    // arbitrary frame would have to find the enclosing keyframe first.
    if (frame != 0)
      return false;
    opus_decoder_ctl(m_opus, OPUS_RESET_STATE);
    m_leftover.clear();
    m_leftover_pos = 0;
    m_pre_skip_remaining = m_pre_skip;
    m_block = nullptr;
    m_frame_index = 0;
    m_entry = nullptr;
    m_cluster = m_segment != nullptr ? m_segment->GetFirst() : nullptr;
    return true;
  }

private:
  // Decodes the next Opus packet into m_leftover. False only at end of stream;
  // a packet that fails to decode is skipped with m_leftover left empty, so the
  // caller's loop pulls the next one.
  bool decode_next() {
    const u8 *data = nullptr;
    long len = 0;
    if (!next_packet(data, len))
      return false;

    const int got = opus_decode(m_opus, data, nx::cast<opus_int32>(len),
                                m_pcm.data(), OPUS_MAX_FRAME, 0);
    if (got <= 0)
      return true; // skip a bad packet, try the next

    const u32 ch = m_format.channels;
    u32 samples = nx::cast<u32>(got); // per channel
    u32 start = 0;
    if (m_pre_skip_remaining > 0) {
      const u32 drop = nx::min(m_pre_skip_remaining, samples);
      start = drop;
      m_pre_skip_remaining -= drop;
    }
    m_leftover_pos = 0;
    if (start < samples)
      m_leftover.assign(m_pcm.data() + nx::cast<usize>(start) * ch,
                        m_pcm.data() + nx::cast<usize>(samples) * ch);
    else
      m_leftover.clear();
    return true;
  }

  // Yields the next Opus packet on the audio track, honouring block lacing
  // (Opus routinely stores several packets in one block). False at end.
  bool next_packet(const u8 *&data, long &len) {
    for (;;) {
      if (m_block != nullptr && m_frame_index < m_block->GetFrameCount()) {
        const mkvparser::Block::Frame &frame = m_block->GetFrame(m_frame_index++);
        if (frame.len <= 0)
          continue;
        m_packet.resize(nx::cast<usize>(frame.len));
        if (frame.Read(&m_reader, m_packet.data()) < 0)
          continue;
        data = m_packet.data();
        len = frame.len;
        return true;
      }
      if (!advance_block())
        return false;
    }
  }

  bool advance_block() {
    while (m_cluster != nullptr && !m_cluster->EOS()) {
      long status = 0;
      if (m_entry == nullptr)
        status = m_cluster->GetFirst(m_entry);
      else {
        const mkvparser::BlockEntry *next = nullptr;
        status = m_cluster->GetNext(m_entry, next);
        m_entry = next;
      }
      if (status < 0 || m_entry == nullptr || m_entry->EOS()) {
        m_cluster = m_segment->GetNext(m_cluster);
        m_entry = nullptr;
        continue;
      }
      const mkvparser::Block *const block = m_entry->GetBlock();
      if (block == nullptr || block->GetTrackNumber() != m_track_number)
        continue;
      m_block = block;
      m_frame_index = 0;
      return true;
    }
    return false;
  }

  nx::blob<u8> m_bytes;
  MemoryReader m_reader{nullptr, 0};
  mkvparser::Segment *m_segment = nullptr;
  const mkvparser::Cluster *m_cluster = nullptr;
  const mkvparser::BlockEntry *m_entry = nullptr;
  const mkvparser::Block *m_block = nullptr;
  int m_frame_index = 0;

  OpusDecoder *m_opus = nullptr;
  nxe::audio::SoundFormat m_format;
  long long m_track_number = 0;
  u32 m_pre_skip = 0;
  u32 m_pre_skip_remaining = 0;

  nx::vector<u8> m_packet;
  nx::vector<i16> m_pcm; // opus_decode target; sized once, never on the stack
  nx::vector<i16> m_leftover;
  usize m_leftover_pos = 0;
};

} // namespace

nxe::audio::DecoderPtr open_webm_opus(const nx::string_view path) {
  auto decoder = std::make_unique<WebmOpusDecoder>();
  if (!decoder->open(path))
    return nullptr;
  return decoder;
}

} // namespace nxm::video
