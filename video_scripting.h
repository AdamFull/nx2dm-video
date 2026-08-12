#pragma once

/**
 * @file video_scripting.h
 * @brief What a script may do to a clip (namespace nxm::video).
 */

namespace nxe {
class ModuleContext;
namespace script {
class Host;
}
} // namespace nxe

namespace nxm::video {

void expose_video_services(nxe::script::Host &host, nxe::ModuleContext &ctx);

} // namespace nxm::video
