#pragma once

namespace nxe {
class ModuleContext;
namespace script {
class Host;
}
}

namespace nxm::video {

void expose_video_services(nxe::script::Host &host, nxe::ModuleContext &ctx);

}
