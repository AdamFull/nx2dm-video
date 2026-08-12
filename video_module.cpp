/**
 * @file video_module.cpp
 * @brief What tells an Engine about video, and the only file here that knows an
 * Engine exists.
 */

#include "video/video_audio.h"
#include "video/video_clip.h"
#include "video/video_component.h"
#include "video/video_decode.h"
#include "video/video_hw.h"
#include "video/video_mediacodec.h"
#include "video/video_pass.h"
#include "video/video_place.h"
#include "video/video_scripting.h"
#include "video/video_source.h"

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

// V1 has no container yet: the synthetic source stands in for a real decode so
// the whole engine path can be exercised. Fixed, cheap dimensions.
constexpr u32 SYNTH_WIDTH = 640;
constexpr u32 SYNTH_HEIGHT = 360;
constexpr f64 SYNTH_FPS = 30.0;

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

/// One clip's decoded frame for this frame, handed from the simulation to the
/// renderer through the frame packet - which is per-frame, so nothing here is
/// shared across the frames in flight and nothing races.
struct VideoItem {
  nxe::scene::Entity owner{};
  glm::vec4 rect{-1.f, -1.f, 1.f, 1.f};
  bool world_space = false;
  glm::vec2 eye{0.f, 0.f};
  f32 half_h = 1.f;
  f64 pts = 0.0;
  VideoFrame frame;
  // The hardware path carries no CPU frame: the render side decodes it, and
  // needs the clip to open its decoder and the show-time (pts) to pace it.
  bool hw = false;
  bool looping = false;
  nx::string clip;
  std::atomic<bool> *hw_finished = nullptr;
};

struct VideoChannel {
  nx::vector<VideoItem> items;
  void clear() { items.clear(); }
};

struct Active {
  nxe::scene::Entity entity{};
  VideoPlayer *player = nullptr;
};

/// A clip's decode and playback state. Simulation-thread only: created and
/// advanced by the present system, never touched by the renderer. The stream is
/// heap-held so a seek can retire it and swap in a fresh one without moving the
/// object the mixer still points a playing voice at.
struct Decoder {
  nxe::scene::Entity owner{};
  PacedPlayback playback;
  nx::unique_ptr<nxe::audio::AudioStream> audio;
  nxe::audio::VoiceHandle voice;
  u64 last_dsp = 0;
  bool audio_started = false;
  bool paused = false;
  bool touched = false;
  // Hardware clips decode render-side, so the simulation only keeps the clock:
  // it hands the render thread a show-time, never a frame.
  bool initialized = false;
  bool hw = false;
  f64 hw_clock = 0.0;
  // The WebM the player's clip resolves to (a .nxvid names one; a raw .webm is
  // itself) and its loop default, resolved once at init.
  nx::string source;
  bool loop = true;
  // Set by the render thread when a non-looping hardware clip ends; read here.
  std::atomic<bool> hw_finished{false};
};

/// A stream a seek replaced, kept alive until its old voice has drained it - the
/// audio thread may still be reading its ring.
struct DyingStream {
  nx::unique_ptr<nxe::audio::AudioStream> stream;
  nxe::audio::VoiceHandle voice;
};

struct Planes {
  nxe::scene::Entity owner{};
  nxe::rhi::TextureHandle luma;   ///< R8, full resolution
  nxe::rhi::TextureHandle chroma; ///< R8G8 interleaved Cb,Cr, half resolution
  u32 width = 0;
  u32 height = 0;
  f64 uploaded_pts = -1.0;
  bool touched = false;
};

/// A clip's hardware decoder. Render-thread only: it owns the device video
/// decoder, so it cannot live sim-side like the CPU path's Decoder does.
struct HwClip {
  nxe::scene::Entity owner{};
  HwVideoSource source;
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
    for (Planes &p : m_planes)
      destroy_planes(ctx.device(), p);
    m_planes.clear();
    // The hardware clips own device video decoders; drop them while the device
    // is still up (each decoder waits the GPU idle as it tears down).
    m_hw_clips.clear();
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

    // One shared budget across the clips, handed out in a rotating order so no
    // clip is starved when several want to decode the same frame.
    i32 remaining = nx::cast<i32>(m_decode_budget);
    i32 *const budget = m_decode_budget > 0 ? &remaining : nullptr;
    const usize count = active.size();
    for (usize k = 0; k < count; ++k) {
      const Active &a = active[(m_decode_cursor + k) % count];
      VideoPlayer &player = *a.player;
      Decoder &decoder = decoder_for(a.entity);
      if (!decoder.initialized) {
        decoder.initialized = true;
        const ResolvedClip resolved = resolve_clip(player);
        decoder.source = resolved.source;
        decoder.loop = resolved.loop;
        decoder.hw = hw_decode_available(ctx.device()) &&
                     webm_is_vp9(decoder.source);
        if (!decoder.hw) {
          SourcePtr source;
          if (!decoder.source.empty()) {
            // Android's hardware block, when it can read this clip; nullptr on
            // every other platform, so the CPU decoder takes over below.
            source = open_media_codec(decoder.source);
            if (!source)
              source = open_webm(decoder.source);
          }
          if (!source)
            source = std::make_unique<SyntheticSource>(SYNTH_WIDTH, SYNTH_HEIGHT,
                                                       SYNTH_FPS);
          decoder.playback.reset(std::move(source), decoder.loop);
        }
        if (m_audio_enabled)
          start_audio(ctx, decoder);
      }

      if (player.seek_to >= 0.0) {
        if (decoder.hw)
          decoder.hw_clock = player.seek_to;
        else
          (void)decoder.playback.seek(player.seek_to);
        if (decoder.audio_started)
          reseat_audio(ctx, decoder, player.seek_to);
        player.seek_to = -1.0;
      }

      f64 step = 0.0;
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
        step = advance_clock(ctx, decoder, dt);
      }

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

      if (decoder.hw) {
        // No decode here - the render side does it. Just carry the clock forward
        // and hand over the show-time; end-of-stream comes back through the
        // atomic the render thread wrote last tick.
        decoder.hw_clock += step;
        player.position = decoder.hw_clock;
        player.finished = decoder.hw_finished.load(std::memory_order_relaxed);
        item.hw = true;
        item.clip = decoder.source;
        item.looping = decoder.loop;
        item.pts = decoder.hw_clock;
        item.hw_finished = &decoder.hw_finished;
        channel.items.push_back(std::move(item));
        continue;
      }

      const VideoFrame *const frame = decoder.playback.advance(step, budget);
      player.finished = decoder.playback.finished();
      if (frame == nullptr)
        continue;
      player.position = frame->pts;
      item.pts = frame->pts;
      item.frame = *frame; // copied into the per-frame packet, race-free
      channel.items.push_back(std::move(item));
    }
    if (count != 0)
      ++m_decode_cursor;

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
    for (Planes &p : m_planes)
      p.touched = false;
    for (nx::unique_ptr<HwClip> &c : m_hw_clips)
      c->touched = false;

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

      if (item.hw) {
        HwClip &clip = hw_clip_for(item.owner);
        if (!clip.opened) {
          clip.opened = true;
          clip.failed = !clip.source.open(device, item.clip);
        }
        if (clip.failed)
          continue;
        const HwFrame frame = clip.source.frame_at(item.pts, item.looping);
        // Report end-of-stream back to the simulation (it reads it next tick).
        if (item.hw_finished != nullptr)
          item.hw_finished->store(clip.source.finished(),
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
        continue;
      }

      Planes &planes = planes_for(device, item.owner, item.frame);
      if (!planes.luma.valid())
        continue;
      // The present system re-emits the current frame every tick; only a new
      // one is worth the copy to the GPU.
      if (planes.uploaded_pts != item.pts) {
        upload(device, planes, item.frame);
        planes.uploaded_pts = item.pts;
      }

      VideoDraw draw;
      draw.rect = rect;
      draw.luma = planes.luma;
      draw.chroma = planes.chroma;
      draw.sampler_index = m_sampler;
      draw.colour = item.frame.colour;
      draws.push_back(draw);
    }

    reap_planes(device);
    reap_hw_clips();

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

  [[nodiscard]] Planes &planes_for(nxe::rhi::Device &device,
                                   const nxe::scene::Entity entity,
                                   const VideoFrame &frame) {
    Planes *found = nullptr;
    for (Planes &p : m_planes)
      if (p.owner == entity) {
        found = &p;
        break;
      }
    if (found == nullptr) {
      m_planes.push_back(Planes{});
      found = &m_planes.back();
      found->owner = entity;
    }
    found->touched = true;
    ensure_planes(device, *found, frame);
    return *found;
  }

  [[nodiscard]] HwClip &hw_clip_for(const nxe::scene::Entity entity) {
    for (nx::unique_ptr<HwClip> &c : m_hw_clips)
      if (c->owner == entity) {
        c->touched = true;
        return *c;
      }
    m_hw_clips.push_back(nx::make_unique<HwClip>());
    m_hw_clips.back()->owner = entity;
    m_hw_clips.back()->touched = true;
    return *m_hw_clips.back();
  }

  void reap_hw_clips() {
    for (usize i = m_hw_clips.size(); i-- > 0;)
      if (!m_hw_clips[i]->touched) {
        if (i != m_hw_clips.size() - 1)
          m_hw_clips[i] = std::move(m_hw_clips.back());
        m_hw_clips.pop_back();
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

  void reap_planes(nxe::rhi::Device &device) {
    for (usize i = m_planes.size(); i-- > 0;)
      if (!m_planes[i].touched) {
        destroy_planes(device, m_planes[i]);
        if (i != m_planes.size() - 1)
          m_planes[i] = std::move(m_planes.back());
        m_planes.pop_back();
      }
  }

  static void ensure_planes(nxe::rhi::Device &device, Planes &planes,
                            const VideoFrame &frame) {
    if (planes.width == frame.width && planes.height == frame.height &&
        planes.luma.valid())
      return;
    destroy_planes(device, planes);
    planes.luma = make_texture(device, frame.width, frame.height,
                               nxe::rhi::Format::R8_UNORM, "video.luma");
    planes.chroma =
        make_texture(device, frame.chroma_width(), frame.chroma_height(),
                     nxe::rhi::Format::RG8_UNORM, "video.chroma");
    if (!planes.luma.valid() || !planes.chroma.valid()) {
      destroy_planes(device, planes);
      return;
    }
    planes.width = frame.width;
    planes.height = frame.height;
  }

  [[nodiscard]] static nxe::rhi::TextureHandle
  make_texture(nxe::rhi::Device &device, const u32 width, const u32 height,
               const nxe::rhi::Format format, const nx::string_view name) {
    nxe::rhi::TextureDesc desc{};
    desc.name = name;
    desc.format = format;
    desc.width = nx::max(width, 1u);
    desc.height = nx::max(height, 1u);
    desc.usage =
        nxe::rhi::TextureUsage::Sampled | nxe::rhi::TextureUsage::CopyDst;
    return device.create_texture(desc);
  }

  static void destroy_planes(nxe::rhi::Device &device, Planes &planes) {
    for (nxe::rhi::TextureHandle *tex : {&planes.luma, &planes.chroma}) {
      if (tex->valid())
        device.destroy_texture(*tex);
      *tex = {};
    }
    planes.width = 0;
    planes.height = 0;
    planes.uploaded_pts = -1.0; // fresh textures need the next frame uploaded
  }

  void upload(nxe::rhi::Device &device, const Planes &planes,
              const VideoFrame &frame) {
    upload_texture(device, planes.luma, frame.y.data(), frame.y.size(),
                   frame.width, frame.height, frame.width);
    // Interleave the two chroma planes into the one R8G8 plane the shader
    // samples, so a CPU frame takes the NV12 shape the hardware path decodes to.
    interleave_chroma(frame, m_chroma_scratch);
    if (!m_chroma_scratch.empty())
      upload_texture(device, planes.chroma, m_chroma_scratch.data(),
                     m_chroma_scratch.size(), frame.chroma_width(),
                     frame.chroma_height(), frame.chroma_width() * 2);
  }

  static void upload_texture(nxe::rhi::Device &device,
                             const nxe::rhi::TextureHandle tex, const u8 *data,
                             const usize size, const u32 width, const u32 height,
                             const u32 row_pitch) {
    if (data == nullptr || size == 0 || !tex.valid())
      return;
    nxe::rhi::ImageSubresource sub{};
    sub.offset = 0;
    sub.size = nx::cast<u64>(size);
    sub.width = width;
    sub.height = height;
    sub.depth = 1;
    sub.row_pitch = row_pitch;
    const nxe::rhi::ImageSubresource layout[1] = {sub};
    (void)device.uploader().upload_texture(
        tex, std::span<const u8>(data, size),
        std::span<const nxe::rhi::ImageSubresource>(layout, 1));
  }

  VideoRenderer m_renderer;
  nx::vector<nx::unique_ptr<Decoder>> m_decoders;
  // Decoders whose entity is gone but whose voice the audio thread may still be
  // rendering. Kept until the mixer says the voice has ended, then dropped.
  nx::vector<nx::unique_ptr<Decoder>> m_dying;
  // Streams a seek replaced, kept until their old voices drain.
  nx::vector<DyingStream> m_dying_streams;
  nx::vector<Planes> m_planes;
  // Reused each upload to interleave Cb,Cr into the NV12 chroma plane.
  nx::vector<u8> m_chroma_scratch;
  nx::vector<nx::unique_ptr<HwClip>> m_hw_clips;
  u32 m_sampler = 0;
  /// Frames decoded per present tick across all clips; 0 means no cap. Rotated
  /// over by m_decode_cursor so the budget reaches every clip in turn.
  u32 m_decode_budget = 0;
  u32 m_decode_cursor = 0;
  bool m_can_draw = false;
  bool m_audio_enabled = true;
};

} // namespace
} // namespace nxm::video

NX_DECLARE_MODULE(video, nxm::video::VideoModule)
