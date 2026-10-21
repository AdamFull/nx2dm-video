#pragma once

#include "nx/gfx/device.h"

#include <utility>

namespace nx::gfx::test {

/// A test's device: made by init(), freed by shutdown() or with the owner,
/// and usable wherever an nx::gfx::device is. Only use it after init()
/// succeeded.
class OwnedDevice {
public:
  OwnedDevice() = default;
  ~OwnedDevice() { shutdown(); }

  OwnedDevice(const OwnedDevice &) = delete;
  OwnedDevice &operator=(const OwnedDevice &) = delete;

  [[nodiscard]] bool init(const nx::gfx::device_desc &desc) {
    shutdown();
    auto made = nx::gfx::create_device(desc);
    if (!made) {
      m_error = made.error();
      return false;
    }
    m_device = made.value();
    return true;
  }

  void shutdown() noexcept {
    nx::gfx::destroy_device(std::exchange(m_device, nullptr));
  }

  [[nodiscard]] const nx::gfx::device_init_error &error() const noexcept {
    return m_error;
  }

  operator nx::gfx::device &() const noexcept { return *m_device; }
  [[nodiscard]] nx::gfx::device *get() const noexcept { return m_device; }

private:
  nx::gfx::device *m_device = nullptr;
  nx::gfx::device_init_error m_error;
};

} // namespace nx::gfx::test
