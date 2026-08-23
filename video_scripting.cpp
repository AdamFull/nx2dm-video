#include "video/video_scripting.h"

#include "video/video_component.h"

#include "core/app/engine.h"
#include "core/script/script_host.h"

namespace nxm::video {
namespace {

namespace sys = nxe::sys;

[[nodiscard]] VideoPlayer *player_of(nxe::ModuleContext &ctx,
                                     const sys::Entity e) {
  if (e == sys::Entity{})
    return nullptr;
  return ctx.scene().registry().try_get<VideoPlayer>(e);
}

}

void expose_video_services(nxe::script::Host &host, nxe::ModuleContext &ctx) {
  host.expose_as("video_play", [&ctx](const sys::Entity e) {
    VideoPlayer *const player = player_of(ctx, e);
    if (player == nullptr)
      return false;
    player->autoplay = true;
    return true;
  });

  host.expose_as("video_stop", [&ctx](const sys::Entity e) {
    VideoPlayer *const player = player_of(ctx, e);
    if (player == nullptr)
      return false;
    player->autoplay = false;
    return true;
  });

  host.expose_as("video_finished", [&ctx](const sys::Entity e) {
    const VideoPlayer *const player = player_of(ctx, e);
    return player == nullptr || player->finished;
  });

  host.expose_as("video_looping", [&ctx](const sys::Entity e, const bool on) {
    VideoPlayer *const player = player_of(ctx, e);
    if (player == nullptr)
      return false;
    player->looping = on;
    return true;
  });

  host.expose_as("video_seek", [&ctx](const sys::Entity e, const f32 seconds) {
    VideoPlayer *const player = player_of(ctx, e);
    if (player == nullptr)
      return false;
    player->seek_to = seconds < 0.f ? 0.0 : nx::cast<f64>(seconds);
    return true;
  });

  host.expose_as("video_position", [&ctx](const sys::Entity e) {
    const VideoPlayer *const player = player_of(ctx, e);
    return player == nullptr ? 0.f : nx::cast<f32>(player->position);
  });

  host.expose_as("video_pause", [&ctx](const sys::Entity e, const bool on) {
    VideoPlayer *const player = player_of(ctx, e);
    if (player == nullptr)
      return false;
    player->paused = on;
    return true;
  });
}

}
