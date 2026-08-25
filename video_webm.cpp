#include "video/video_webm.h"

#include "video/video_asset.h"

#include <cmath>
#include <cstring>
#include <memory>

namespace nxm::video {
namespace {

[[nodiscard]] bool fail(nx::string &error, const nx::string_view message) {
  error = nx::string(message);
  return false;
}

[[nodiscard]] bool supported_video_codec(const char *const codec) noexcept {
  return codec != nullptr && (std::strcmp(codec, "V_VP9") == 0 ||
                              std::strcmp(codec, "V_AV01") == 0 ||
                              std::strcmp(codec, "V_AV1") == 0);
}

[[nodiscard]] bool supported_audio_codec(const char *const codec) noexcept {
  return codec != nullptr && (std::strcmp(codec, "A_OPUS") == 0 ||
                              std::strcmp(codec, "A_VORBIS") == 0);
}

[[nodiscard]] bool contains_track(const nx::vector<long long> &tracks,
                                  const long long number) noexcept {
  for (const long long track : tracks)
    if (track == number)
      return true;
  return false;
}

[[nodiscard]] bool vorbis_header(const u8 *const data, const usize size,
                                 const u8 kind) noexcept {
  return size >= 7 && data != nullptr && data[0] == kind &&
         std::memcmp(data + 1, "vorbis", 6) == 0;
}

[[nodiscard]] bool validate_audio_private(const mkvparser::AudioTrack &track,
                                          nx::string &error) {
  size_t size = 0;
  const u8 *const data = track.GetCodecPrivate(size);
  const char *const codec = track.GetCodecId();
  const long long channels = track.GetChannels();
  if (codec != nullptr && std::strcmp(codec, "A_OPUS") == 0) {
    if (data == nullptr || size < 19 || size > MAX_VIDEO_CODEC_PRIVATE_BYTES ||
        std::memcmp(data, "OpusHead", 8) != 0 ||
        nx::cast<long long>(data[9]) != channels || data[18] != 0)
      return fail(error, "the WebM Opus setup header is invalid");
    return true;
  }
  if (codec == nullptr || std::strcmp(codec, "A_VORBIS") != 0)
    return false;
  if (data == nullptr || size < 3 || size > MAX_VIDEO_CODEC_PRIVATE_BYTES ||
      data[0] != 2)
    return fail(error, "the WebM Vorbis setup headers are invalid");

  usize at = 1;
  usize lengths[2] = {};
  for (usize &length : lengths) {
    while (at < size && data[at] == 255) {
      if (length > MAX_VIDEO_CODEC_PRIVATE_BYTES - 255)
        return fail(error, "the WebM Vorbis setup headers are invalid");
      length += 255;
      ++at;
    }
    if (at >= size)
      return fail(error, "the WebM Vorbis setup headers are invalid");
    length += data[at++];
  }
  if (lengths[0] > size - at)
    return fail(error, "the WebM Vorbis setup headers are truncated");
  const usize second = at + lengths[0];
  if (lengths[1] > size - second)
    return fail(error, "the WebM Vorbis setup headers are truncated");
  const usize third = second + lengths[1];
  if (!vorbis_header(data + at, lengths[0], 1) ||
      !vorbis_header(data + second, lengths[1], 3) ||
      !vorbis_header(data + third, size - third, 5) || lengths[0] < 16 ||
      nx::cast<long long>(data[at + 11]) != channels)
    return fail(error, "the WebM Vorbis setup headers are invalid");
  const u32 sample_rate = nx::cast<u32>(data[at + 12]) |
                          (nx::cast<u32>(data[at + 13]) << 8) |
                          (nx::cast<u32>(data[at + 14]) << 16) |
                          (nx::cast<u32>(data[at + 15]) << 24);
  if (sample_rate == 0)
    return fail(error, "the WebM Vorbis sample rate is invalid");
  return true;
}

} // namespace

bool validate_webm(const std::span<const u8> bytes,
                   nx::string &error) noexcept {
  error.clear();
  if (bytes.empty())
    return fail(error, "the WebM payload is empty");
  if (bytes.size() > MAX_ENCODED_VIDEO_BYTES)
    return fail(error, "the WebM payload exceeds the encoded-size limit");

  try {
    MemoryReader reader(bytes);
    mkvparser::EBMLHeader header;
    long long position = 0;
    if (header.Parse(&reader, position) < 0 || header.m_docType == nullptr ||
        std::strcmp(header.m_docType, "webm") != 0)
      return fail(error, "the payload is not a WebM document");

    mkvparser::Segment *raw_segment = nullptr;
    if (mkvparser::Segment::CreateInstance(&reader, position, raw_segment) <
            0 ||
        raw_segment == nullptr)
      return fail(error, "the WebM segment cannot be opened");
    const std::unique_ptr<mkvparser::Segment> segment(raw_segment);
    if (segment->Load() < 0)
      return fail(error, "the WebM segment is truncated or malformed");

    const mkvparser::Tracks *const tracks = segment->GetTracks();
    if (tracks == nullptr)
      return fail(error, "the WebM document has no tracks");

    nx::vector<long long> relevant_tracks;
    relevant_tracks.reserve(tracks->GetTracksCount());
    for (unsigned long i = 0; i < tracks->GetTracksCount(); ++i) {
      const mkvparser::Track *const track = tracks->GetTrackByIndex(i);
      if (track == nullptr || track->GetNumber() <= 0)
        return fail(error, "the WebM document has invalid track metadata");

      size_t private_size = 0;
      (void)track->GetCodecPrivate(private_size);
      if (private_size > MAX_VIDEO_CODEC_PRIVATE_BYTES)
        return fail(error, "a WebM codec-private payload is too large");

      if (track->GetType() == mkvparser::Track::kVideo) {
        if (!supported_video_codec(track->GetCodecId()))
          return fail(error, "the WebM video codec is not VP9 or AV1");
        const auto *const video =
            static_cast<const mkvparser::VideoTrack *>(track);
        const long long width = video->GetWidth();
        const long long height = video->GetHeight();
        if (width <= 0 || height <= 0 ||
            !valid_video_dimensions(nx::cast<u64>(width),
                                    nx::cast<u64>(height)))
          return fail(error, "the WebM video dimensions are invalid");
        const f64 frame_rate = video->GetFrameRate();
        if (std::isfinite(frame_rate) && frame_rate > MAX_VIDEO_FRAME_RATE)
          return fail(error, "the WebM frame rate exceeds the runtime limit");
        relevant_tracks.push_back(track->GetNumber());
      } else if (track->GetType() == mkvparser::Track::kAudio) {
        if (!supported_audio_codec(track->GetCodecId()))
          return fail(error, "the WebM audio codec is not Opus or Vorbis");
        const auto *const audio =
            static_cast<const mkvparser::AudioTrack *>(track);
        if (audio->GetChannels() < 1 || audio->GetChannels() > 2)
          return fail(error,
                      "the WebM audio track must have one or two channels");
        if (!validate_audio_private(*audio, error))
          return false;
        relevant_tracks.push_back(track->GetNumber());
      }
    }
    if (relevant_tracks.empty())
      return fail(error, "the WebM document has no supported media track");

    nx::vector<u8> scratch;
    bool found_media_packet = false;
    for (const mkvparser::Cluster *cluster = segment->GetFirst();
         cluster != nullptr && !cluster->EOS();
         cluster = segment->GetNext(cluster)) {
      const mkvparser::BlockEntry *entry = nullptr;
      long status = cluster->GetFirst(entry);
      while (status >= 0 && entry != nullptr && !entry->EOS()) {
        const mkvparser::Block *const block = entry->GetBlock();
        if (block != nullptr &&
            contains_track(relevant_tracks, block->GetTrackNumber())) {
          if (block->GetFrameCount() <= 0)
            return fail(error, "a WebM block has no frames");
          found_media_packet = true;
          for (int frame_index = 0; frame_index < block->GetFrameCount();
               ++frame_index) {
            const mkvparser::Block::Frame &frame = block->GetFrame(frame_index);
            if (frame.len <= 0 ||
                nx::cast<usize>(frame.len) > MAX_VIDEO_PACKET_BYTES)
              return fail(error, "a WebM packet has an invalid size");
            scratch.resize(nx::cast<usize>(frame.len));
            if (frame.Read(&reader, scratch.data()) < 0)
              return fail(error, "a WebM packet is truncated");
          }
        }

        const mkvparser::BlockEntry *next = nullptr;
        status = cluster->GetNext(entry, next);
        entry = next;
      }
      if (status < 0)
        return fail(error, "the WebM cluster structure is malformed");
    }
    if (!found_media_packet)
      return fail(error, "the WebM media tracks have no packets");
    return true;
  } catch (...) {
    return fail(error, "resource exhaustion while validating WebM");
  }
}

} // namespace nxm::video
