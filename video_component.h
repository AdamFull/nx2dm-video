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
  bool fullscreen = true;
  bool world_space = false;
  glm::vec4 rect{-1.f, -1.f, 1.f, 1.f};
  bool finished = false;
  f64 seek_to = -1.0;
  f64 position = 0.0;
};

} // namespace nxm::video
