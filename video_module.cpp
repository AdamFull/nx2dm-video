/**
 * @file video_module.cpp
 * @brief What tells an Engine about video, and the only file here that knows an
 * Engine exists.
 */

#include "video/video_audio.h"
#include "video/video_clip.h"
#include "video/video_component.h"
#include "video/video_gpu.h"
#include "video/video_pass.h"
#include "video/video_place.h"
#include "video/video_scripting.h"

#include "core/app/engine.h"
#include "core/app/module.h"
#include "core/audio/mixer.h"
#include "core/audio/stream.h"
#include "core/scene/components.h"
#include "core/scene/sampler.h"
#include "core/scene/scene_json.h"

#include "core/foundation/containers/blob.h"
#include "core/foundation/containers/small_vector.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/diagnostics/log.h"

#include <atomic>
#include <span>
#include <utility>

namespace nxm::video {
namespace {

constexpr nx::string_view COMPONENT = "VideoPlayer";
constexpr nx::string_view PRESENT_SYSTEM = "video.present";
constexpr nx::string_view DRAW_PASS = "video.draw";
constexpr nx::string_view WORLD_SLOT = "world";
constexpr nx::string_view SHADER = "video/video";

struct ResolvedClip {
  nx::string source;
  bool loop = true;
};

[[nodiscard]] ResolvedClip resolve_clip(const VideoPlayer &player) {
  if (nx::string_view(player.clip).ends_with(".nxvid")) {
    VideoClip clip;
    if (load_video_clip(player.clip, clip))
      return {clip.source, clip.loop};
    return {{}, player.looping};
  }
  return {player.clip, player.looping};
}

struct VideoItem {
  nxe::scene::Entity owner{};
  glm::vec4 rect{-1.f, -1.f, 1.f, 1.f};
  bool world_space = false;
  glm::vec2 eye{0.f, 0.f};
  f32 half_h = 1.f;
  f64 pts = 0.0;
  bool looping = false;
  nx::string clip;
  std::atomic<bool> *finished = nullptr;
};

struct VideoChannel {
  nx::vector<VideoItem> items;
  void clear() { items.clear(); }
};

struct Active {
  nxe::scene::Entity entity{};
  VideoPlayer *player = nullptr;
};

/// A clip's simulation-side state: the playback clock and the audio voice. The
/// video decode lives render-side now (see Source), so this never touches the
/// device - it advances the clock the render thread paces the picture against,
/// and reads end-of-stream back through the atomic the render thread writes. The
/// stream is heap-held so a seek can retire it and swap in a fresh one without
/// moving the object the mixer still points a playing voice at.
struct Decoder {
  nxe::scene::Entity owner{};
  nx::unique_ptr<nxe::audio::AudioStream> audio;
  nxe::audio::VoiceHandle voice;
  u64 last_dsp = 0;
  bool audio_started = false;
  bool paused = false;
  bool touched = false;
  bool initialized = false;
  f64 clock = 0.0;
  // The WebM the player's clip resolves to (a .nxvid names one; a raw .webm is
  // itself) and its loop default, resolved once at init.
  nx::string source;
  bool loop = true;
  // Written by the render thread when a non-looping clip ends; read here.
  std::atomic<bool> finished{false};
};

/// A stream a seek replaced, kept alive until its old voice has drained it - the
/// audio thread may still be reading its ring.
struct DyingStream {
  nx::unique_ptr<nxe::audio::AudioStream> stream;
  nxe::audio::VoiceHandle voice;
};

struct Source {
  nxe::scene::Entity owner{};
  GpuSourcePtr source;
  bool opened = false;
  bool failed = false;
  bool touched = false;
};

class VideoModule final : public nxe::Module {
public:
  [[nodiscard]] nxe::ModuleDescriptor descriptor() const noexcept override {
    nxe::ModuleDescriptor out{};
    out.id = "video";
    out.version = {1, 0, 0};
    return out;
  }

  bool on_register(nxe::ModuleContext &ctx) override {
    ctx.scene().registry().register_component<VideoPlayer>({.name = COMPONENT});
    ctx.scene().formats().add(
        nxe::scene::described<VideoPlayer>("video", "video_players"));
    return true;
  }

  void on_expose_scripts(nxe::script::Host &host,
                         nxe::ModuleContext &ctx) override {
    expose_video_services(host, ctx);
  }

  bool on_attach(nxe::ModuleContext &ctx) override {
    m_can_draw = m_renderer.init(ctx.device(), ctx.load_shader(SHADER));
    if (!m_can_draw)
      nx::logw("video: no renderer; clips will not draw");
    m_sampler = ctx.samplers().index(nxe::scene::sampler_bilinear());

    // Simulation: decode a frame per clip into the packet. No device here - the
    // uploader is the render thread's alone (see vk_upload.h).
    ctx.schedule().define(
        PRESENT_SYSTEM,
        nxe::sys::SystemFn([this, &ctx](const nxe::sys::Context &c) {
          present(ctx, nx::cast<f64>(c.dt));
        }));
    ctx.schedule().add(nxe::sys::Stage::Present, PRESENT_SYSTEM);

    if (!m_can_draw)
      return true;

    // Renderer: upload this frame's planes and draw. All GPU work lives here.
    ctx.passes().define(
        DRAW_PASS, nxe::PassFn([this, &ctx](nxe::rg::RenderGraph &graph,
                                            nxe::RenderContext &context) {
          record(ctx, graph, context);
        }));

    static constexpr nx::string_view MINE[] = {DRAW_PASS};
    if (!ctx.fill_pass_slot(WORLD_SLOT, MINE, name()))
      nx::logw("video: nothing to fill; the frame has no '{}' slot", WORLD_SLOT);

    nx::logi("video: attached");
    return true;
  }

  void on_detach(nxe::ModuleContext &ctx) override {
    // Stop the voices, but do not free the streams here: on_detach runs before
    // the engine stops the audio device (Engine::shutdown), so the audio thread
    // may still be mid-block. The decoders outlive this call and are freed when
    // the module is - by which point the device, and its thread, are gone.
    if (ctx.mixer().valid()) {
      for (nx::unique_ptr<Decoder> &d : m_decoders)
        if (d->audio_started)
          ctx.mixer().stop(d->voice);
      for (nx::unique_ptr<Decoder> &d : m_dying)
        ctx.mixer().stop(d->voice);
      for (DyingStream &d : m_dying_streams)
        ctx.mixer().stop(d.voice);
    }
    // The sources own device textures or a video decoder; drop them while the
    // device is still up (a hardware decoder waits the GPU idle as it tears
    // down, a CPU source frees its plane textures).
    m_sources.clear();
    m_renderer.shutdown(ctx.device());
  }

private:
  void present(nxe::ModuleContext &ctx, const f64 dt) {
    nxe::r2d::FramePacket *const packet = ctx.frame_packet();
    if (packet == nullptr)
      return;
    VideoChannel &channel = packet->channel<VideoChannel>();
    channel.clear();

    for (nx::unique_ptr<Decoder> &d : m_decoders)
      d->touched = false;

    glm::vec2 cam_eye{0.f, 0.f};
    f32 cam_half_h = 1.f;
    if (const nxe::scene::Entity cam = ctx.scene().active_camera();
        cam.valid()) {
      auto &reg = ctx.scene().registry();
      if (const auto *const c = reg.try_get<nxe::scene::Camera2D>(cam)) {
        const f32 zoom = c->zoom > 0.f ? c->zoom : 1.f;
        cam_half_h = c->ortho_height * 0.5f / zoom;
      }
      if (const auto *const w = reg.try_get<nxe::scene::WorldTransform2D>(cam))
        cam_eye = glm::vec2(w->world[2][0], w->world[2][1]);
    }

    nx::small_vector<Active, 8> active;
    ctx.scene().registry().view<VideoPlayer>().each(
        [&](const nxe::scene::Entity entity, VideoPlayer &player) {
          // Stopped: leave any decoder untouched so reap releases it (and its
          // voice), and show nothing.
          if (!player.autoplay) {
            player.finished = false;
            return;
          }
          active.push_back({entity, &player});
        });

    // Uniform across every clip: the simulation carries the clock and hands the
    // render thread a show-time. It never decodes and never touches the device -
    // that is all render-side now, the same for hardware and software.
    for (const Active &a : active) {
      VideoPlayer &player = *a.player;
      Decoder &decoder = decoder_for(a.entity);
      if (!decoder.initialized) {
        decoder.initialized = true;
        const ResolvedClip resolved = resolve_clip(player);
        decoder.source = resolved.source;
        decoder.loop = resolved.loop;
        if (m_audio_enabled)
          start_audio(ctx, decoder);
      }

      if (player.seek_to >= 0.0) {
        decoder.clock = player.seek_to; // the render side seeks its decoder to it
        if (decoder.audio_started)
          reseat_audio(ctx, decoder, player.seek_to);
        player.seek_to = -1.0;
      }

      if (player.paused) {
        if (decoder.audio_started && !decoder.paused) {
          if (ctx.mixer().valid())
            ctx.mixer().set_paused(decoder.voice, true);
          decoder.paused = true;
        }
      } else {
        if (decoder.audio_started && decoder.paused) {
          if (ctx.mixer().valid())
            ctx.mixer().set_paused(decoder.voice, false);
          decoder.paused = false;
          decoder.last_dsp = ctx.mixer().dsp_frame();
        }
        decoder.clock += advance_clock(ctx, decoder, dt);
      }

      player.position = decoder.clock;
      // End-of-stream comes back through the atomic the render thread wrote last
      // tick - the one clean render->simulation signal the packet model lacks.
      player.finished = decoder.finished.load(std::memory_order_relaxed);

      VideoItem item;
      item.owner = a.entity;
      item.world_space = player.world_space;
      if (player.world_space) {
        item.rect = player.rect; // world-space AABB; the pass projects it
        item.eye = cam_eye;
        item.half_h = cam_half_h;
      } else {
        item.rect = player.fullscreen ? glm::vec4{-1.f, -1.f, 1.f, 1.f}
                                      : player.rect;
      }
      item.pts = decoder.clock;
      item.clip = decoder.source;
      item.looping = decoder.loop;
      item.finished = &decoder.finished;
      channel.items.push_back(std::move(item));
    }

    reap_decoders(ctx);
    sweep_dying(ctx);
    sweep_dying_streams(ctx);
  }

  // The clock the picture is paced against. With sound it is the mixer's DSP
  // clock, so the video chases the audio and a hitch in one drags the other;
  // without, it is the frame delta. Pumping the ring is done here too - once per
  // frame keeps the decoder ahead of the device.
  [[nodiscard]] f64 advance_clock(nxe::ModuleContext &ctx, Decoder &decoder,
                                  const f64 dt) {
    if (!decoder.audio_started || !ctx.mixer().valid())
      return dt;
    decoder.audio->pump();
    const u32 rate = ctx.mixer().config().sample_rate;
    const u64 now = ctx.mixer().dsp_frame();
    const u64 prev = decoder.last_dsp;
    decoder.last_dsp = now;
    return rate != 0 ? nx::cast<f64>(now - prev) / rate : dt;
  }

  void record(nxe::ModuleContext &ctx, nxe::rg::RenderGraph &graph,
              nxe::RenderContext &context) {
    const VideoChannel *const channel =
        context.packet != nullptr ? context.packet->find_channel<VideoChannel>()
                                  : nullptr;
    if (channel == nullptr || channel->items.empty())
      return;

    nxe::rhi::Device &device = ctx.device();
    for (nx::unique_ptr<Source> &s : m_sources)
      s->touched = false;

    const f32 aspect = context.extent.height != 0
                           ? nx::cast<f32>(context.extent.width) /
                                 nx::cast<f32>(context.extent.height)
                           : 1.f;

    nx::vector<VideoDraw> draws;
    draws.reserve(channel->items.size());
    for (const VideoItem &item : channel->items) {
      const glm::vec4 rect =
          item.world_space
              ? world_box_to_ndc(item.rect, item.eye, item.half_h, aspect)
              : item.rect;

      Source &clip = source_for(item.owner);
      if (!clip.opened) {
        clip.opened = true;
        clip.source = create_video_source(device, item.clip);
        clip.failed = clip.source == nullptr;
      }
      if (clip.source == nullptr)
        continue;

      const GpuFrame frame = clip.source->frame_at(item.pts, item.looping);
      // Report end-of-stream back to the simulation (it reads it next tick).
      if (item.finished != nullptr)
        item.finished->store(clip.source->finished(),
                             std::memory_order_relaxed);
      if (!frame.valid())
        continue;

      VideoDraw draw;
      draw.rect = rect;
      draw.uv_scale = frame.uv_scale;
      draw.luma = frame.luma;
      draw.chroma = frame.chroma;
      draw.sampler_index = m_sampler;
      draw.colour = frame.colour;
      draws.push_back(draw);
    }

    reap_sources();

    const nxe::rhi::Format format =
        ctx.config().scene_format == nxe::rhi::Format::Unknown
            ? device.swapchain_format()
            : ctx.config().scene_format;
    m_renderer.draw(device, graph, context.target(nxe::TARGET_SCENE_COLOR),
                    format, std::span<const VideoDraw>(draws.data(), draws.size()));
  }

  [[nodiscard]] Decoder &decoder_for(const nxe::scene::Entity entity) {
    for (nx::unique_ptr<Decoder> &d : m_decoders)
      if (d->owner == entity) {
        d->touched = true;
        return *d;
      }
    m_decoders.push_back(nx::make_unique<Decoder>());
    m_decoders.back()->owner = entity;
    m_decoders.back()->touched = true;
    return *m_decoders.back();
  }

  [[nodiscard]] Source &source_for(const nxe::scene::Entity entity) {
    for (nx::unique_ptr<Source> &s : m_sources)
      if (s->owner == entity) {
        s->touched = true;
        return *s;
      }
    m_sources.push_back(nx::make_unique<Source>());
    m_sources.back()->owner = entity;
    m_sources.back()->touched = true;
    return *m_sources.back();
  }

  // A source untouched this frame - its clip stopped or its node is gone - is
  // dropped, and with it the device textures or decoder it owns. Render-thread
  // only, so the device is up when its resources are freed.
  void reap_sources() {
    for (usize i = m_sources.size(); i-- > 0;)
      if (!m_sources[i]->touched) {
        if (i != m_sources.size() - 1)
          m_sources[i] = std::move(m_sources.back());
        m_sources.pop_back();
      }
  }

  void start_audio(nxe::ModuleContext &ctx, Decoder &decoder,
                   const f64 start_seconds = 0.0) {
    if (decoder.source.empty() || !ctx.config().audio || !ctx.mixer().valid())
      return;
    nxe::audio::DecoderPtr codec = open_webm_audio(decoder.source);
    if (!codec)
      return;
    if (start_seconds > 0.0 && codec->format().sample_rate != 0)
      (void)codec->seek(
          nx::cast<u64>(start_seconds * codec->format().sample_rate));
    auto stream = nx::make_unique<nxe::audio::AudioStream>();
    if (!stream->open(std::move(codec), 1.f, decoder.loop))
      return;
    stream->pump(); // prime the ring before the voice starts
    decoder.audio = std::move(stream);
    decoder.voice = ctx.mixer().play(*decoder.audio, {});
    decoder.audio_started = true;
    decoder.paused = false; // a fresh voice starts running
    decoder.last_dsp = ctx.mixer().dsp_frame();
  }

  // Move a playing clip's sound to @p seek_seconds: retire the current stream to
  // the graveyard, where its voice may still be draining it, and start a fresh
  // one seeked there. Each stream owns its ring, so the two never share one.
  void reseat_audio(nxe::ModuleContext &ctx, Decoder &decoder,
                    const f64 seek_seconds) {
    if (ctx.mixer().valid())
      ctx.mixer().stop(decoder.voice);
    m_dying_streams.push_back({std::move(decoder.audio), decoder.voice});
    decoder.voice = {};
    decoder.audio_started = false;
    start_audio(ctx, decoder, seek_seconds);
  }

  // Dead entities with a playing voice cannot be freed yet: the audio thread may
  // still touch the stream. Stop the voice and set it aside; sweep_dying frees
  // it once the mixer confirms it has ended.
  void reap_decoders(nxe::ModuleContext &ctx) {
    for (usize i = m_decoders.size(); i-- > 0;) {
      if (m_decoders[i]->touched)
        continue;
      if (m_decoders[i]->audio_started && ctx.mixer().valid()) {
        ctx.mixer().stop(m_decoders[i]->voice);
        m_dying.push_back(std::move(m_decoders[i]));
      }
      if (i != m_decoders.size() - 1)
        m_decoders[i] = std::move(m_decoders.back());
      m_decoders.pop_back();
    }
  }

  void sweep_dying(nxe::ModuleContext &ctx) {
    const bool mixer_gone = !ctx.mixer().valid();
    for (usize i = m_dying.size(); i-- > 0;)
      if (mixer_gone || !ctx.mixer().is_playing(m_dying[i]->voice)) {
        if (i != m_dying.size() - 1)
          m_dying[i] = std::move(m_dying.back());
        m_dying.pop_back();
      }
  }

  void sweep_dying_streams(nxe::ModuleContext &ctx) {
    const bool mixer_gone = !ctx.mixer().valid();
    for (usize i = m_dying_streams.size(); i-- > 0;)
      if (mixer_gone || !ctx.mixer().is_playing(m_dying_streams[i].voice)) {
        if (i != m_dying_streams.size() - 1)
          m_dying_streams[i] = std::move(m_dying_streams.back());
        m_dying_streams.pop_back();
      }
  }

  VideoRenderer m_renderer;
  nx::vector<nx::unique_ptr<Decoder>> m_decoders;
  // Decoders whose entity is gone but whose voice the audio thread may still be
  // rendering. Kept until the mixer says the voice has ended, then dropped.
  nx::vector<nx::unique_ptr<Decoder>> m_dying;
  // Streams a seek replaced, kept until their old voices drain.
  nx::vector<DyingStream> m_dying_streams;
  // One decode source per clip - hardware or CPU behind GpuVideoSource -
  // render-side, owning its textures or its device decoder.
  nx::vector<nx::unique_ptr<Source>> m_sources;
  u32 m_sampler = 0;
  bool m_can_draw = false;
  bool m_audio_enabled = true;
};

} // namespace
} // namespace nxm::video

NX_DECLARE_MODULE(video, nxm::video::VideoModule)
