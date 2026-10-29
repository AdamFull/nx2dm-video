#include "video/video_gpu.h"

#include "video/video_decode.h"
#include "video/video_demux.h"
#include "video/video_hw.h"
#include "video/video_mediacodec.h"

#include "nx/gfx/gfx.h"

#include "core/foundation/threading/thread_pool.h"

#include <cmath>
#include <memory>
#include <span>

namespace gfx = nx::gfx;

namespace nxm::video {
namespace {

constexpr u32 SYNTH_WIDTH = 640;
constexpr u32 SYNTH_HEIGHT = 360;
constexpr f64 SYNTH_FPS = 30.0;

class CpuVideoSource final : public GpuVideoSource {
public:
  ~CpuVideoSource() override { destroy_textures(); }

  [[nodiscard]] bool open(gfx::device &device, SourcePtr source,
                          nx::thread_pool *const workers) {
    m_device = &device;
    m_workers = workers;
    if (!source)
      return false;
    m_width = source->width();
    m_height = source->height();
    m_fps = source->frame_rate();
    m_playback = nx::make_shared<PacedPlayback>();
    m_playback->reset(std::move(source), true);
    return true;
  }

  [[nodiscard]] bool valid() const noexcept override {
    return m_playback != nullptr && m_playback->valid();
  }
  [[nodiscard]] u32 width() const noexcept override { return m_width; }
  [[nodiscard]] u32 height() const noexcept override { return m_height; }
  [[nodiscard]] f64 frame_rate() const noexcept override { return m_fps; }
  [[nodiscard]] bool seek(const f64 target_seconds,
                          const bool looping) override {
    if (!std::isfinite(target_seconds))
      return false;
    const f64 target = nx::max(target_seconds, 0.0);
    // A decode in flight is still moving the playback.
    if (m_decoding.valid()) {
      m_decoding.wait();
      m_decoding = {};
    }
    const f64 duration = m_playback->duration();
    const f64 local =
        looping && duration > 0.0 ? std::fmod(target, duration) : target;
    if (!m_playback->seek(local, target))
      return false;
    m_finished = false;
    return true;
  }
  [[nodiscard]] f64 position() const noexcept override { return m_position; }
  [[nodiscard]] bool finished() const noexcept override { return m_finished; }

  [[nodiscard]] GpuFrame frame_at(const f64 target_seconds,
                                  const bool looping) override {
    if (!std::isfinite(target_seconds))
      return m_frame;
    if (m_workers == nullptr) {
      m_playback->set_looping(looping);
      show(m_playback->advance_to(target_seconds));
      return m_frame;
    }
    // Uploads what the last call's decode made and starts the next on a
    // worker: the picture is a frame behind its target, and the render job
    // never decodes.
    if (m_decoding.valid()) {
      if (!m_decoding.ready())
        return m_frame;
      const VideoFrame *const decoded =
          m_decoding.failed() ? nullptr : m_decoding.get();
      m_decoding = {};
      show(decoded);
    }
    m_playback->set_looping(looping);
    m_decoding = m_workers->async(
        nx::task_priority::Normal,
        [playback = m_playback, target_seconds]() -> const VideoFrame * {
          return playback->advance_to(target_seconds);
        });
    return m_frame;
  }

private:
  void show(const VideoFrame *const frame) {
    m_finished = m_playback->finished();
    if (frame == nullptr)
      return;
    if (frame->pts != m_uploaded_pts || !m_frame.valid()) {
      if (upload(*frame)) {
        m_uploaded_pts = frame->pts;
        m_position = frame->pts;
        m_frame.luma = m_luma;
        m_frame.chroma = m_chroma;
        m_frame.uv_scale = {1.f, 1.f};
        m_frame.colour = frame->colour;
      }
    }
  }

  [[nodiscard]] bool upload(const VideoFrame &frame) {
    if (!ensure_textures(frame))
      return false;
    const bool luma = upload_plane(m_luma, frame.y.data(), frame.y.size(),
                                   frame.width, frame.height, frame.width);
    if (!interleave_chroma(frame, m_chroma_scratch))
      return false;
    const bool chroma = upload_plane(
        m_chroma, m_chroma_scratch.data(), m_chroma_scratch.size(),
        frame.chroma_width(), frame.chroma_height(), frame.chroma_width() * 2);
    return luma && chroma;
  }

  [[nodiscard]] bool ensure_textures(const VideoFrame &frame) {
    if (m_tex_width == frame.width && m_tex_height == frame.height &&
        m_luma.valid())
      return true;
    destroy_textures();
    m_luma = make_texture(frame.width, frame.height, gfx::format::R8_UNORM,
                          "video.cpu.luma");
    m_chroma = make_texture(frame.chroma_width(), frame.chroma_height(),
                            gfx::format::RG8_UNORM, "video.cpu.chroma");
    if (!m_luma.valid() || !m_chroma.valid()) {
      destroy_textures();
      return false;
    }
    m_tex_width = frame.width;
    m_tex_height = frame.height;
    return true;
  }

  [[nodiscard]] gfx::texture_handle
  make_texture(const u32 width, const u32 height, const gfx::format format,
               const nx::string_view name) const {
    gfx::texture_desc desc{};
    desc.name = name;
    desc.format = format;
    desc.width = nx::max(width, 1u);
    desc.height = nx::max(height, 1u);
    desc.usage = gfx::texture_usage::Sampled | gfx::texture_usage::CopyDst;
    return gfx::create_texture(*m_device, desc);
  }

  [[nodiscard]] bool upload_plane(const gfx::texture_handle tex,
                                  const u8 *const data, const usize size,
                                  const u32 width, const u32 height,
                                  const u32 row_pitch) const {
    if (data == nullptr || size == 0 || !tex.valid())
      return false;
    gfx::image_subresource sub{};
    sub.offset = 0;
    sub.size = nx::cast<u64>(size);
    sub.width = width;
    sub.height = height;
    sub.depth = 1;
    sub.row_pitch = row_pitch;
    const gfx::image_subresource layout[1] = {sub};
    return gfx::upload_texture(
               *m_device, tex, std::span<const u8>(data, size),
               std::span<const gfx::image_subresource>(layout, 1))
        .ok();
  }

  void destroy_textures() noexcept {
    if (m_device != nullptr) {
      if (m_luma.valid())
        gfx::destroy_texture(*m_device, m_luma);
      if (m_chroma.valid())
        gfx::destroy_texture(*m_device, m_chroma);
    }
    m_luma = {};
    m_chroma = {};
    m_tex_width = 0;
    m_tex_height = 0;
    m_frame = {};
    m_uploaded_pts = -1.0;
  }

  gfx::device *m_device = nullptr;
  nx::thread_pool *m_workers = nullptr;
  /// Shared with a decode in flight, which outlives the source when it is
  /// dropped mid-decode.
  nx::shared_ptr<PacedPlayback> m_playback;
  nx::future<const VideoFrame *> m_decoding;
  gfx::texture_handle m_luma;
  gfx::texture_handle m_chroma;
  nx::vector<u8> m_chroma_scratch;
  GpuFrame m_frame;
  u32 m_width = 0;
  u32 m_height = 0;
  u32 m_tex_width = 0;
  u32 m_tex_height = 0;
  f64 m_fps = 30.0;
  f64 m_position = 0.0;
  f64 m_uploaded_pts = -1.0;
  bool m_finished = false;
};

} // namespace

OpenedVideo::OpenedVideo() noexcept = default;
OpenedVideo::~OpenedVideo() = default;
OpenedVideo::OpenedVideo(OpenedVideo &&) noexcept = default;
OpenedVideo &OpenedVideo::operator=(OpenedVideo &&) noexcept = default;

OpenedVideo open_video(const nx::string_view path, const bool hardware) {
  OpenedVideo out;
  if (path.empty()) {
    out.cpu =
        std::make_unique<SyntheticSource>(SYNTH_WIDTH, SYNTH_HEIGHT, SYNTH_FPS);
    return out;
  }
  // The hardware block where it can take the clip (VP9 only, for now), the
  // CPU decoder otherwise.
  if (hardware) {
    auto demux = std::make_unique<WebmVideoDemux>();
    if (demux->open(path, "V_VP9")) {
      out.vp9 = std::move(demux);
      return out;
    }
  }
  out.cpu = open_media_codec(path);
  if (!out.cpu)
    out.cpu = open_webm(path);
  return out;
}

GpuSourcePtr create_video_source(gfx::device &device, OpenedVideo opened,
                                 nx::thread_pool *const workers) {
  if (opened.vp9 != nullptr) {
    auto hw = std::make_unique<HwVideoSource>();
    if (hw->open(device, std::move(opened.vp9)))
      return hw;
    return nullptr;
  }
  auto cpu = std::make_unique<CpuVideoSource>();
  if (cpu->open(device, std::move(opened.cpu), workers))
    return cpu;
  return nullptr;
}

} // namespace nxm::video
