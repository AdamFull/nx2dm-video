
#include "framework/nxtest.h"

#include "app/engine.h"
#include "script/script_host.h"
#include "video/video_scripting.h"

namespace {

namespace script = nxe::script;

struct Exposed {
  nxe::Engine engine{nxe::Game{}};
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

}

TEST_CASE("video scripting: every service is exposed with the shape a script "
          "is told about") {
  const Exposed exposed;

  static constexpr struct {
    nx::string_view name;
    nx::string_view signature;
  } WANT[] = {
      {"video_play", "(number)->(boolean)"},
      {"video_stop", "(number)->(boolean)"},
      {"video_finished", "(number)->(boolean)"},
      {"video_looping", "(number,boolean)->(boolean)"},
      {"video_seek", "(number,number)->(boolean)"},
      {"video_position", "(number)->(number)"},
      {"video_pause", "(number,boolean)->(boolean)"},
  };

  CHECK(exposed.services.size() == nx::array_size(WANT));
  for (const auto &want : WANT) {
    const script::Host::ServiceInfo *const found = exposed.find(want.name);
    REQUIRE(found != nullptr);
    CHECK(found->signature == want.signature);
  }
}
