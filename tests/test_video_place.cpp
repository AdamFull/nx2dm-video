/**
 * @file test_video_place.cpp
 * @brief World-space placement: a clip's world-space box projects to the same
 * NDC rect the Camera2D would put a sprite at.
 *
 * Pure math, no device. The trap is the y direction and the aspect: get either
 * wrong and a world-placed clip lands off-screen or squashed, which a headless
 * render test would only catch as a moved rectangle. Here the expected NDC is
 * hand-computed from the projection scene_renderer builds.
 */

#include "framework/nxtest.h"

#include "video/video_place.h"

#include <cmath>

using nxm::video::world_box_to_ndc;

namespace {

constexpr f32 EPS = 1e-5f;

[[nodiscard]] bool near4(const glm::vec4 &a, const glm::vec4 &b) {
  return std::fabs(a.x - b.x) < EPS && std::fabs(a.y - b.y) < EPS &&
         std::fabs(a.z - b.z) < EPS && std::fabs(a.w - b.w) < EPS;
}

} // namespace

TEST_CASE("place: a box the size of the view fills NDC") {
  // half_h 5, aspect 2 -> half_w 10; the full extent is (-10,-5)..(10,5).
  const glm::vec4 ndc =
      world_box_to_ndc({-10.f, -5.f, 10.f, 5.f}, {0.f, 0.f}, 5.f, 2.f);
  CHECK(near4(ndc, {-1.f, -1.f, 1.f, 1.f}));
}

TEST_CASE("place: a corner box lands in a corner, aspect included") {
  // Same camera; the box is the top-right quarter of the view.
  const glm::vec4 ndc =
      world_box_to_ndc({0.f, 0.f, 10.f, 5.f}, {0.f, 0.f}, 5.f, 2.f);
  CHECK(near4(ndc, {0.f, 0.f, 1.f, 1.f}));
}

TEST_CASE("place: the camera's eye recentres the box") {
  // eye (10,20), half_h 4, aspect 1.5 -> half_w 6; a box at the eye of half
  // the extent maps to the top-right quadrant regardless of where the eye is.
  const glm::vec4 ndc =
      world_box_to_ndc({10.f, 20.f, 16.f, 24.f}, {10.f, 20.f}, 4.f, 1.5f);
  CHECK(near4(ndc, {0.f, 0.f, 1.f, 1.f}));
}

TEST_CASE("place: y keeps its sign so the clip is not flipped or offset") {
  // A box entirely below the eye stays below it in NDC (negative y), and one
  // above stays above. A sign slip here is exactly the upside-down bug.
  const glm::vec4 below =
      world_box_to_ndc({-1.f, -5.f, 1.f, -1.f}, {0.f, 0.f}, 5.f, 1.f);
  CHECK(below.y < 0.f);
  CHECK(below.w < 0.f);
  const glm::vec4 above =
      world_box_to_ndc({-1.f, 1.f, 1.f, 5.f}, {0.f, 0.f}, 5.f, 1.f);
  CHECK(above.y > 0.f);
  CHECK(above.w > 0.f);
}

TEST_CASE("place: a degenerate camera collapses rather than dividing by zero") {
  const glm::vec4 ndc =
      world_box_to_ndc({-1.f, -1.f, 1.f, 1.f}, {0.f, 0.f}, 0.f, 1.f);
  CHECK(ndc.y == 0.f);
  CHECK(ndc.w == 0.f);
}
