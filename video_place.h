#pragma once

/**
 * @file video_place.h
 * @brief Placing a clip's quad (namespace nxm::video).
 */

#include "core/foundation/core/foundation.h"

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

namespace nxm::video {

[[nodiscard]] inline glm::vec4 world_box_to_ndc(const glm::vec4 &world,
                                                const glm::vec2 &eye,
                                                const f32 half_h,
                                                const f32 aspect) noexcept {
  const f32 half_w = half_h * aspect;
  const f32 inv_w = half_w != 0.f ? 1.f / half_w : 0.f;
  const f32 inv_h = half_h != 0.f ? 1.f / half_h : 0.f;
  return glm::vec4((world.x - eye.x) * inv_w, (world.y - eye.y) * inv_h,
                   (world.z - eye.x) * inv_w, (world.w - eye.y) * inv_h);
}

} // namespace nxm::video
