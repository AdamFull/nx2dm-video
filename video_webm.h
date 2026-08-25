#pragma once

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

#include "mkvparser/mkvparser.h"

#include <cstring>
#include <span>

namespace nxm::video {

class MemoryReader final : public mkvparser::IMkvReader {
public:
  explicit MemoryReader(const std::span<const u8> bytes) noexcept
      : m_data(bytes.data()), m_size(nx::cast<long long>(bytes.size())) {}
  MemoryReader(const u8 *data, const long long size) noexcept
      : m_data(data), m_size(nx::max(size, 0ll)) {}

  int Read(const long long pos, const long len,
           unsigned char *const buffer) override {
    if (pos < 0 || len < 0 || pos > m_size || len > m_size - pos)
      return -1;
    if (len == 0)
      return 0;
    if (buffer == nullptr || m_data == nullptr)
      return -1;
    std::memcpy(buffer, m_data + pos, nx::cast<usize>(len));
    return 0;
  }

  int Length(long long *const total, long long *const available) override {
    if (total != nullptr)
      *total = m_size;
    if (available != nullptr)
      *available = m_size;
    return 0;
  }

private:
  const u8 *m_data = nullptr;
  long long m_size = 0;
};

/// Performs host-side structural validation of a WebM asset. It accepts the
/// codecs the runtime can decode (VP9/AV1 video and optional Opus/Vorbis audio)
/// and reads every relevant packet so truncated or oversized sources fail cook.
[[nodiscard]] bool validate_webm(std::span<const u8> bytes,
                                 nx::string &error) noexcept;

} // namespace nxm::video
