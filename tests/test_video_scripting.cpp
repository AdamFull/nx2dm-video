/**
 * @file test_video_scripting.cpp
 * @brief What the module hands a script, and the shape of it.
 *
 * The build writes these signatures into the declarations a type checker reads
 * (script-services.json -> host_api.luau), so a renamed service or a moved
 * argument is a script that quietly stops type-checking, not a build error.
 * This pins the C++ side so the two cannot drift.
 *
 * No backend and no started Engine: Host::expose only records; bind() is what
 * needs a VM.
 */

#include "framework/nxtest.h"

#include "core/app/engine.h"
#include "core/script/script_host.h"
#include "video/video_scripting.h"

namespace {

namespace script = nxe::script;

struct Exposed {
  nxe::Engine engine{nullptr};
  nxe::ModuleContext ctx{engine};
  script::Host host;
  nx::vector<script::Host::ServiceInfo> services;

  Exposed() {
    nxm::video::expose_video_services(host, ctx);
    services = host.services();
  }

  [[nodiscard]] const script::Host::ServiceInfo *
  find(const nx::string_view name) const {
    for (const script::Host::ServiceInfo &one : services)
      if (one.name == name)
        return &one;
    return nullptr;
  }
};

} // namespace

TEST_CASE("video scripting: every service is exposed with the shape a script "
          "is told about") {
  const Exposed exposed;

  // Spelled out rather than counted: a count would let a rename through, and the
  // whole point is that these and the generated declarations cannot drift.
  static constexpr struct {
    nx::string_view name;
    nx::string_view signature;
  } WANT[] = {
      {"video_play", "(number)->(boolean)"},
      {"video_stop", "(number)->(boolean)"},
      {"video_finished", "(number)->(boolean)"},
      {"video_looping", "(number,boolean)->(boolean)"},
  };

  CHECK(exposed.services.size() == nx::array_size(WANT));
  for (const auto &want : WANT) {
    const script::Host::ServiceInfo *const found = exposed.find(want.name);
    REQUIRE(found != nullptr);
    CHECK(found->signature == want.signature);
  }
}
