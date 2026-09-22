
#include "framework/nxtest.h"

#include "app/engine.h"
#include "core/foundation/platform/filesystem.h"
#include "script/luau/luau_bindings.h"
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
};

}

TEST_CASE("video scripting: every service is exposed as script-services.json "
          "declares it") {
  const Exposed exposed;
  const auto manifest = nx::fs::file_read_text(
      nx::fs::path_view(NX_MODULE_SERVICES_MANIFEST));
  REQUIRE(manifest);

  nx::string error;
  if (!script::luau_manifest_agrees(manifest.value(), exposed.services, error))
    FAIL(error.c_str());
}
