/**
 * @file video_module.cpp
 * @brief What tells an Engine about video, and the only file here that knows an
 * Engine exists.
 */

#include "video/video_audio.h"
#include "video/video_component.h"
#include "video/video_decode.h"
#include "video/video_pass.h"
#include "video/video_source.h"

#include "core/app/engine.h"
#include "core/app/module.h"
#include "core/audio/mixer.h"
#include "core/audio/stream.h"
#include "core/scene/sampler.h"
#include "core/scene/scene_json.h"

#include "core/foundation/containers/blob.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/diagnostics/log.h"

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

/// One clip's decoded frame for this frame, handed from the simulation to the
/// renderer through the frame packet - which is per-frame, so nothing here is
/// shared across the frames in flight and nothing races.
struct VideoItem {
  nxe::scene::Entity owner{};
  glm::vec4 rect{-1.f, -1.f, 1.f, 1.f};
  f64 pts = 0.0;
  VideoFrame frame;
};

struct VideoChannel {
  nx::vector<VideoItem> items;
  void clear() { items.clear(); }
};

/// A clip's decode and playback state. Simulation-thread only: created and
/// advanced by the present system, never touched by the renderer. Held behind a
/// pointer (see m_decoders) so its address is stable: the mixer keeps a pointer
/// to `audio`, which must not move while a voice is playing it.
struct Decoder {
  nxe::scene::Entity owner{};
  PacedPlayback playback;
  nxe::audio::AudioStream audio;
  nxe::audio::VoiceHandle voice;
  u64 last_dsp = 0;
  bool audio_started = false;
  bool touched = false;
};

/// A clip's GPU textures. Render-thread only: created, uploaded and destroyed
/// inside the pass, never touched by the simulation.
struct Planes {
  nxe::scene::Entity owner{};
  nxe::rhi::TextureHandle y;
  nxe::rhi::TextureHandle cb;
  nxe::rhi::TextureHandle cr;
  u32 width = 0;
  u32 height = 0;
  f64 uploaded_pts = -1.0;
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
    }
    for (Planes &p : m_planes)
      destroy_planes(ctx.device(), p);
    m_planes.clear();
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

    ctx.scene().registry().view<VideoPlayer>().each(
        [&](const nxe::scene::Entity entity, VideoPlayer &player) {
          // Stopped: leave any decoder untouched so reap releases it (and its
          // voice), and show nothing.
          if (!player.autoplay) {
            player.finished = false;
            return;
          }
          Decoder &decoder = decoder_for(entity);
          if (!decoder.playback.valid()) {
            SourcePtr source;
            if (!player.clip.empty())
              source = open_webm(player.clip);
            // No clip, or the file would not open: the synthetic pattern keeps
            // the pipeline exercised rather than drawing nothing.
            if (!source)
              source = std::make_unique<SyntheticSource>(SYNTH_WIDTH,
                                                         SYNTH_HEIGHT,
                                                         SYNTH_FPS);
            decoder.playback.reset(std::move(source), player.looping);
            if (m_audio_enabled)
              start_audio(ctx, decoder, player);
          }

          const f64 step = advance_clock(ctx, decoder, dt);
          const VideoFrame *const frame = decoder.playback.advance(step);
          player.finished = decoder.playback.finished();
          if (frame == nullptr)
            return;

          VideoItem item;
          item.owner = entity;
          item.rect = player.fullscreen ? glm::vec4{-1.f, -1.f, 1.f, 1.f}
                                        : player.rect;
          item.pts = frame->pts;
          item.frame = *frame; // copied into the per-frame packet, race-free
          channel.items.push_back(std::move(item));
        });

    reap_decoders(ctx);
    sweep_dying(ctx);
  }

  // The clock the picture is paced against. With sound it is the mixer's DSP
  // clock, so the video chases the audio and a hitch in one drags the other;
  // without, it is the frame delta. Pumping the ring is done here too - once per
  // frame keeps the decoder ahead of the device.
  [[nodiscard]] f64 advance_clock(nxe::ModuleContext &ctx, Decoder &decoder,
                                  const f64 dt) {
    if (!decoder.audio_started || !ctx.mixer().valid())
      return dt;
    decoder.audio.pump();
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

    nx::vector<VideoDraw> draws;
    draws.reserve(channel->items.size());
    for (const VideoItem &item : channel->items) {
      Planes &planes = planes_for(device, item.owner, item.frame);
      if (!planes.y.valid())
        continue;
      // The present system re-emits the current frame every tick; only a new
      // one is worth the copy to the GPU.
      if (planes.uploaded_pts != item.pts) {
        upload(device, planes, item.frame);
        planes.uploaded_pts = item.pts;
      }

      VideoDraw draw;
      draw.rect = item.rect;
      draw.y_plane = device.texture_index(planes.y);
      draw.cb_plane = device.texture_index(planes.cb);
      draw.cr_plane = device.texture_index(planes.cr);
      draw.sampler_index = m_sampler;
      draws.push_back(draw);
    }

    reap_planes(device);

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

  // A clip with audio: open the Opus track and hand it to the mixer. The video
  // then chases the audio clock (see present). Silent clips, and any project
  // with audio off, skip this and stay on the frame delta.
  void start_audio(nxe::ModuleContext &ctx, Decoder &decoder,
                   const VideoPlayer &player) {
    if (player.clip.empty() || !ctx.config().audio || !ctx.mixer().valid())
      return;
    nxe::audio::DecoderPtr codec = open_webm_opus(player.clip);
    if (!codec)
      return;
    if (!decoder.audio.open(std::move(codec), 1.f, player.looping))
      return;
    decoder.audio.pump(); // prime the ring before the voice starts
    decoder.voice = ctx.mixer().play(decoder.audio, {});
    decoder.audio_started = true;
    decoder.last_dsp = ctx.mixer().dsp_frame();
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
        planes.y.valid())
      return;
    destroy_planes(device, planes);
    planes.y = make_plane(device, frame.width, frame.height, "video.y");
    planes.cb = make_plane(device, frame.chroma_width(), frame.chroma_height(),
                           "video.cb");
    planes.cr = make_plane(device, frame.chroma_width(), frame.chroma_height(),
                           "video.cr");
    if (!planes.y.valid() || !planes.cb.valid() || !planes.cr.valid()) {
      destroy_planes(device, planes);
      return;
    }
    planes.width = frame.width;
    planes.height = frame.height;
  }

  [[nodiscard]] static nxe::rhi::TextureHandle
  make_plane(nxe::rhi::Device &device, const u32 width, const u32 height,
             const nx::string_view name) {
    nxe::rhi::TextureDesc desc{};
    desc.name = name;
    desc.format = nxe::rhi::Format::R8_UNORM;
    desc.width = nx::max(width, 1u);
    desc.height = nx::max(height, 1u);
    desc.usage =
        nxe::rhi::TextureUsage::Sampled | nxe::rhi::TextureUsage::CopyDst;
    return device.create_texture(desc);
  }

  static void destroy_planes(nxe::rhi::Device &device, Planes &planes) {
    for (nxe::rhi::TextureHandle *tex : {&planes.y, &planes.cb, &planes.cr}) {
      if (tex->valid())
        device.destroy_texture(*tex);
      *tex = {};
    }
    planes.width = 0;
    planes.height = 0;
    planes.uploaded_pts = -1.0; // fresh textures need the next frame uploaded
  }

  static void upload(nxe::rhi::Device &device, const Planes &planes,
                     const VideoFrame &frame) {
    upload_plane(device, planes.y, frame.y, frame.width, frame.height);
    upload_plane(device, planes.cb, frame.cb, frame.chroma_width(),
                 frame.chroma_height());
    upload_plane(device, planes.cr, frame.cr, frame.chroma_width(),
                 frame.chroma_height());
  }

  static void upload_plane(nxe::rhi::Device &device,
                           const nxe::rhi::TextureHandle tex,
                           const nx::vector<u8> &data, const u32 width,
                           const u32 height) {
    if (data.empty() || !tex.valid())
      return;
    nxe::rhi::ImageSubresource sub{};
    sub.offset = 0;
    sub.size = nx::cast<u64>(data.size());
    sub.width = width;
    sub.height = height;
    sub.depth = 1;
    sub.row_pitch = width; // R8: one byte per texel, tight.
    const nxe::rhi::ImageSubresource layout[1] = {sub};
    (void)device.uploader().upload_texture(
        tex, std::span<const u8>(data.data(), data.size()),
        std::span<const nxe::rhi::ImageSubresource>(layout, 1));
  }

  VideoRenderer m_renderer;
  nx::vector<nx::unique_ptr<Decoder>> m_decoders;
  // Decoders whose entity is gone but whose voice the audio thread may still be
  // rendering. Kept until the mixer says the voice has ended, then dropped.
  nx::vector<nx::unique_ptr<Decoder>> m_dying;
  nx::vector<Planes> m_planes;
  u32 m_sampler = 0;
  bool m_can_draw = false;
  bool m_audio_enabled = true;
};

} // namespace
} // namespace nxm::video

NX_DECLARE_MODULE(video, nxm::video::VideoModule)
