#pragma once

#include "core/foundation/core/foundation.h"

#include "core/foundation/math/names.h"

namespace nxm::video {

[[nodiscard]] inline float4 world_box_to_ndc(const float4 &world,
                                             const float2 &eye,
                                             const f32 half_h,
                                             const f32 aspect) noexcept {
  const f32 half_w = half_h * aspect;
  const f32 inv_w = half_w != 0.f ? 1.f / half_w : 0.f;
  const f32 inv_h = half_h != 0.f ? 1.f / half_h : 0.f;
  return float4((world.x - eye.x) * inv_w, (world.y - eye.y) * inv_h,
                (world.z - eye.x) * inv_w, (world.w - eye.y) * inv_h);
}
}
