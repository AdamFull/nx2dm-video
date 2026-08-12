#pragma once

/**
 * @file video_component.h
 * @brief A video clip placed in a scene (namespace nxm::video).
 */

#include "core/scene/components.h"

#include <glm/vec4.hpp>

namespace nxm::video {

/// What a scene authors. Survives a save and a load; holds no pointer into
/// anything loaded. The decode state lives module-side, keyed by entity.
struct VideoPlayer {
  nx::string clip;
  bool autoplay = true;
  bool looping = true;
  /// Cover the whole target. When false, `rect` places it in NDC.
  bool fullscreen = true;
  /// Destination as (x0, y0, x1, y1) in NDC; used only when !fullscreen.
  glm::vec4 rect{-1.f, -1.f, 1.f, 1.f};
  bool finished = false;
};

} // namespace nxm::video
