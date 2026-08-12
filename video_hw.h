#pragma once

/**
 * @file video_hw.h
 * @brief The desktop hardware-decode source: Vulkan Video behind the shared
 * GpuVideoSource interface (namespace nxm::video).
 */

#include "video/video_gpu.h"

#include "core/foundation/strings/utf8_string_view.h"

#include <memory>

namespace nxm::video {

class HwVideoSource final : public GpuVideoSource {
public:
  HwVideoSource();
  ~HwVideoSource() override;
  HwVideoSource(const HwVideoSource &) = delete;
  HwVideoSource &operator=(const HwVideoSource &) = delete;

  [[nodiscard]] bool open(nxe::rhi::Device &device,
                          nx::string_view path) override;
  [[nodiscard]] bool valid() const noexcept override;
  [[nodiscard]] u32 width() const noexcept override;
  [[nodiscard]] u32 height() const noexcept override;
  [[nodiscard]] f64 frame_rate() const noexcept override;

  [[nodiscard]] GpuFrame frame_at(f64 target_seconds, bool looping) override;

  [[nodiscard]] f64 position() const noexcept override;
  [[nodiscard]] bool finished() const noexcept override;

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};

} // namespace nxm::video
