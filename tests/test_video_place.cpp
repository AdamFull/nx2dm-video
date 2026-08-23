
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

}

TEST_CASE("place: a box the size of the view fills NDC") {
  const glm::vec4 ndc =
      world_box_to_ndc({-10.f, -5.f, 10.f, 5.f}, {0.f, 0.f}, 5.f, 2.f);
  CHECK(near4(ndc, {-1.f, -1.f, 1.f, 1.f}));
}

TEST_CASE("place: a corner box lands in a corner, aspect included") {
  const glm::vec4 ndc =
      world_box_to_ndc({0.f, 0.f, 10.f, 5.f}, {0.f, 0.f}, 5.f, 2.f);
  CHECK(near4(ndc, {0.f, 0.f, 1.f, 1.f}));
}

TEST_CASE("place: the camera's eye recentres the box") {
  const glm::vec4 ndc =
      world_box_to_ndc({10.f, 20.f, 16.f, 24.f}, {10.f, 20.f}, 4.f, 1.5f);
  CHECK(near4(ndc, {0.f, 0.f, 1.f, 1.f}));
}

TEST_CASE("place: y keeps its sign so the clip is not flipped or offset") {
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
