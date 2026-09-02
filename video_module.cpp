
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
#include "core/foundation/core/hash.h"
#include "core/foundation/diagnostics/log.h"
#include "core/foundation/vfs/vfs.h"

#include <atomic>
#include <span>
#include <utility>

namespace nxm::video {
namespace {

constexpr nx::string_view COMPONENT = "VideoPlayer";
constexpr nx::string_view PRESENT_SYSTEM = "video.present";
constexpr nx::string_view DRAW_PASS = "video.draw";
constexpr nx::string_view MODULE_SLOT = "video";
constexpr nx::string_view WORLD_SLOT = "world";
constexpr nx::string_view SHADER = "video/video";

struct ResolvedClip {
  nx::string source;
  bool loop = true;
  bool valid = false;
};

[[nodiscard]] ResolvedClip resolve_clip(const VideoPlayer &player) {
  if (nx::string_view(player.clip).ends_with(".nxvid")) {
    VideoClip clip;
    if (load_video_clip(player.clip, clip))
      return {clip.source, clip.loop, true};
    return {{}, player.looping, false};
  }
  return {player.clip, player.looping, !player.clip.empty()};
}

[[nodiscard]] u64 file_stamp(const nx::string_view path) noexcept {
  return nx::vfs::file_generation(path);
}

[[nodiscard]] nx::string selected_video_file(const nx::string_view path) {
  if (path.ends_with(".nxb"))
    return nx::string(path);
  nx::string cooked = cooked_video_path(path);
  if (nx::vfs::stat(cooked.view()).exists)
    return cooked;
  return nx::string(path);
}

[[nodiscard]] u64 dependency_stamp(const nx::string_view player_clip,
                                   const nx::string_view source) noexcept {
  nx::fnv1a64 stamp;
  if (player_clip.ends_with(".nxvid")) {
    const nx::string selected = selected_video_file(player_clip);
    stamp.combine(file_stamp(selected.view()));
  }
  const nx::string selected_source = selected_video_file(source);
  return stamp.combine(file_stamp(selected_source.view())).value();
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
  u64 generation = 0;
  nx::shared_ptr<std::atomic<bool>> finished;
};

struct VideoChannel {
  nx::vector<VideoItem> items;
  void clear() { items.clear(); }
};

struct Active {
  nxe::scene::Entity entity{};
  VideoPlayer *player = nullptr;
};

/// The video decode lives render-side now (see Source), so this never touches the device - it
/// advances the clock the render thread paces the picture against, and reads end-of-stream back
/// through the atomic the render thread writes.
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
  nx::string source;
  nx::string player_clip;
  bool player_looping = true;
  bool loop = true;
  u64 source_stamp = 0;
  u64 checked_epoch = 0;
  u64 generation = 1;
  nx::shared_ptr<std::atomic<bool>> finished =
      nx::make_shared<std::atomic<bool>>(false);
};

struct DyingStream {
  nx::unique_ptr<nxe::audio::AudioStream> stream;
  nxe::audio::VoiceHandle voice;
};

struct Source {
  nxe::scene::Entity owner{};
  GpuSourcePtr source;
  nx::string clip;
  u64 generation = 0;
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
    if (m_attached)
      return true;

    m_audio_enabled = ctx.config().audio;
    m_can_draw = m_renderer.init(ctx.device(), ctx.load_shader(SHADER));
    if (!m_can_draw)
      nx::logw("video: no renderer; clips will not draw");
    m_sampler = ctx.samplers().index(nxe::scene::sampler_bilinear());

    if (!ctx.schedule().try_define(
            PRESENT_SYSTEM,
            nxe::sys::SystemFn([this, &ctx](const nxe::sys::Context &c) {
              present(ctx, nx::cast<f64>(c.dt));
            }))) {
      nx::logw("video: system '{}' is already owned by another module",
               PRESENT_SYSTEM);
      m_renderer.shutdown(ctx.device());
      m_can_draw = false;
      return false;
    }
    ctx.schedule().add(nxe::sys::Stage::Present, PRESENT_SYSTEM);

    const void *const pass_owner = ctx.passes().owner_of(DRAW_PASS);
    if (pass_owner != nullptr && pass_owner != this) {
      nx::logw("video: pass '{}' is already owned by another module",
               DRAW_PASS);
      m_renderer.shutdown(ctx.device());
      m_can_draw = false;
      return false;
    }

    ctx.passes().define(
        DRAW_PASS, nxe::PassFn([this, &ctx](nxe::rg::RenderGraph &graph,
                                            nxe::RenderContext &context) {
          record(ctx, graph, context);
        }));

    static constexpr nx::string_view MINE[] = {DRAW_PASS};
    const nx::string_view slot =
        ctx.has_pass_slot(MODULE_SLOT) ? MODULE_SLOT : WORLD_SLOT;
    if (!ctx.fill_pass_slot(slot, MINE, name()))
      nx::logw("video: nothing to fill; the frame has no '{}' slot", slot);

    m_attached = true;
    nx::logi("video: attached");
    return true;
  }

  void on_detach(nxe::ModuleContext &ctx) override {
    if (!m_attached)
      return;
    if (ctx.mixer().valid()) {
      for (nx::unique_ptr<Decoder> &d : m_decoders)
        if (d->audio_started)
          ctx.mixer().stop(d->voice);
      for (nx::unique_ptr<Decoder> &d : m_dying)
        ctx.mixer().stop(d->voice);
      for (DyingStream &d : m_dying_streams)
        ctx.mixer().stop(d.voice);
    }
    for (usize i = m_decoders.size(); i-- > 0;)
      if (!m_decoders[i]->audio_started) {
        if (i != m_decoders.size() - 1)
          m_decoders[i] = std::move(m_decoders.back());
        m_decoders.pop_back();
      }
    m_sources.clear();
    m_renderer.shutdown(ctx.device());
    m_sampler = 0;
    m_can_draw = false;
    m_attached = false;
  }

  void on_suspend(nxe::ModuleContext &) override {
    if (!m_attached)
      return;
    m_sources.clear();
  }

  void on_hot_reload(nxe::ModuleContext &ctx) override {
    ++m_reload_epoch;
    if (!ctx.shader_reloaded(SHADER))
      return;
    const nxe::rhi::ShaderHandle shader = ctx.load_shader(SHADER);
    if (!shader.valid()) {
      nx::logw("video: changed shader is invalid; keeping the last generation");
      return;
    }
    m_can_draw = m_renderer.reload_shader(ctx.device(), shader);
    if (m_can_draw)
      nx::logi("video: renderer shader reloaded");
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
          if (!player.autoplay) {
            player.finished = false;
            return;
          }
          active.push_back({entity, &player});
        });

    for (const Active &a : active) {
      VideoPlayer &player = *a.player;
      Decoder &decoder = decoder_for(a.entity);
      if (!decoder.initialized) {
        decoder.initialized = true;
        const ResolvedClip resolved = resolve_clip(player);
        decoder.source = resolved.source;
        decoder.loop = resolved.loop;
        decoder.player_clip = player.clip;
        player.looping = resolved.loop;
        decoder.player_looping = player.looping;
        decoder.source_stamp =
            dependency_stamp(player.clip.view(), decoder.source.view());
        decoder.checked_epoch = m_reload_epoch;
        if (m_audio_enabled)
          start_audio(ctx, decoder);
      } else if (player.clip != decoder.player_clip) {
        if (decoder.audio_started) {
          if (ctx.mixer().valid())
            ctx.mixer().stop(decoder.voice);
          m_dying_streams.push_back(
              {std::move(decoder.audio), decoder.voice});
          decoder.voice = {};
          decoder.audio_started = false;
        }
        const ResolvedClip resolved = resolve_clip(player);
        decoder.source = resolved.source;
        decoder.loop = resolved.loop;
        decoder.player_clip = player.clip;
        player.looping = resolved.loop;
        decoder.player_looping = player.looping;
        decoder.source_stamp =
            dependency_stamp(player.clip.view(), decoder.source.view());
        decoder.checked_epoch = m_reload_epoch;
        ++decoder.generation;
        decoder.clock = 0.0;
        decoder.finished->store(false, std::memory_order_relaxed);
        if (m_audio_enabled)
          start_audio(ctx, decoder);
      }


      if (decoder.checked_epoch != m_reload_epoch) {
        decoder.checked_epoch = m_reload_epoch;
        const u64 probed =
            dependency_stamp(player.clip.view(), decoder.source.view());
        if (probed != decoder.source_stamp) {
          const ResolvedClip resolved = resolve_clip(player);
          if (!resolved.valid) {
            // Remember the rejected descriptor generation without disturbing
            // the decoder. A subsequent save changes the stamp and retries.
            decoder.source_stamp = probed;
          } else {
            decoder.source = resolved.source;
            decoder.loop = resolved.loop;
            player.looping = resolved.loop;
            decoder.player_looping = resolved.loop;
            decoder.source_stamp = dependency_stamp(
                player.clip.view(), decoder.source.view());
            decoder.finished->store(false, std::memory_order_relaxed);
            ++decoder.generation;
            if (decoder.audio_started)
              reseat_audio(ctx, decoder, decoder.clock);
            else if (m_audio_enabled)
              start_audio(ctx, decoder, decoder.clock);
            nx::logi("video: reloaded '{}' at {:.3f}s", player.clip,
                     decoder.clock);
          }
        }
      }

      if (player.looping != decoder.player_looping) {
        decoder.player_looping = player.looping;
        decoder.loop = player.looping;
        decoder.finished->store(false, std::memory_order_relaxed);
        if (decoder.audio_started)
          reseat_audio(ctx, decoder, decoder.clock);
      }

      if (player.seek_to >= 0.0) {
        decoder.clock = player.seek_to;
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
      player.finished = decoder.finished->load(std::memory_order_relaxed);

      VideoItem item;
      item.owner = a.entity;
      item.world_space = player.world_space;
      if (player.world_space) {
        item.rect = player.rect;
        item.eye = cam_eye;
        item.half_h = cam_half_h;
      } else {
        item.rect = player.fullscreen ? glm::vec4{-1.f, -1.f, 1.f, 1.f}
                                      : player.rect;
      }
      item.pts = decoder.clock;
      item.clip = decoder.source;
      item.generation = decoder.generation;
      item.looping = decoder.loop;
      item.finished = decoder.finished;
      channel.items.push_back(std::move(item));
    }

    reap_decoders(ctx);
    sweep_dying(ctx);
    sweep_dying_streams(ctx);
  }

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
    for (nx::unique_ptr<Source> &s : m_sources)
      s->touched = false;

    const VideoChannel *const channel =
        context.packet != nullptr ? context.packet->find_channel<VideoChannel>()
                                  : nullptr;
    if (channel == nullptr || channel->items.empty()) {
      reap_sources();
      return;
    }

    nxe::rhi::Device &device = ctx.device();

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
      if (clip.clip != item.clip || clip.generation != item.generation) {
        clip.source.reset();
        clip.clip = item.clip;
        clip.generation = item.generation;
        clip.opened = false;
        clip.failed = false;
      }
      if (!clip.opened) {
        clip.opened = true;
        clip.source = create_video_source(device, item.clip);
        clip.failed = clip.source == nullptr;
        if (clip.source != nullptr && item.pts > 0.0)
          (void)clip.source->seek(item.pts, item.looping);
      }
      if (clip.source == nullptr)
        continue;

      const GpuFrame frame = clip.source->frame_at(item.pts, item.looping);
      if (item.finished)
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
    stream->pump();
    const nxe::audio::VoiceHandle voice = ctx.mixer().play(*stream, {});
    if (!voice.valid())
      return;
    decoder.audio = std::move(stream);
    decoder.voice = voice;
    decoder.audio_started = true;
    decoder.paused = false;
    decoder.last_dsp = ctx.mixer().dsp_frame();
  }

  void reseat_audio(nxe::ModuleContext &ctx, Decoder &decoder,
                    const f64 seek_seconds) {
    if (ctx.mixer().valid())
      ctx.mixer().stop(decoder.voice);
    m_dying_streams.push_back({std::move(decoder.audio), decoder.voice});
    decoder.voice = {};
    decoder.audio_started = false;
    start_audio(ctx, decoder, seek_seconds);
  }

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
  nx::vector<nx::unique_ptr<Decoder>> m_dying;
  nx::vector<DyingStream> m_dying_streams;
  nx::vector<nx::unique_ptr<Source>> m_sources;
  u32 m_sampler = 0;
  bool m_can_draw = false;
  bool m_audio_enabled = true;
  bool m_attached = false;
  u64 m_reload_epoch = 1;
};

}
}

NX_DECLARE_MODULE(video, nxm::video::VideoModule)
