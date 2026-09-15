#pragma once

#include "scene/components.h"

#include <glm/vec4.hpp>

namespace nxm::video {

struct VideoPlayer {
  nx::string clip;
  bool autoplay = true;
  bool looping = true;
  bool fullscreen = true;
  bool world_space = false;
  glm::vec4 rect{-1.f, -1.f, 1.f, 1.f};
  bool finished = false;
  bool paused = false;
  f64 seek_to = -1.0;
  f64 position = 0.0;
};

}
