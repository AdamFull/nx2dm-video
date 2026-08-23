#include "video/video_gpu.h"

#include "video/video_decode.h"
#include "video/video_hw.h"
#include "video/video_mediacodec.h"

#include "core/rendering/rhi/descs.h"

#include <cmath>
#include <memory>
#include <span>

namespace nxm::video {
namespace {

constexpr u32 SYNTH_WIDTH = 640;
constexpr u32 SYNTH_HEIGHT = 360;
constexpr f64 SYNTH_FPS = 30.0;

class CpuVideoSource final : public GpuVideoSource {
public:
  ~CpuVideoSource() override { destroy_textures(); }

  [[nodiscard]] bool open(nxe::rhi::Device &device,
                          const nx::string_view path) override {
    m_device = &device;
    SourcePtr source;
    if (!path.empty()) {
      source = open_media_codec(path);
      if (!source)
        source = open_webm(path);
    }
    if (!source)
      source =
          std::make_unique<SyntheticSource>(SYNTH_WIDTH, SYNTH_HEIGHT, SYNTH_FPS);
    m_width = source->width();
    m_height = source->height();
    m_fps = source->frame_rate();
    m_playback.reset(std::move(source), true);
    return true;
  }

  [[nodiscard]] bool valid() const noexcept override {
    return m_playback.valid();
  }
  [[nodiscard]] u32 width() const noexcept override { return m_width; }
  [[nodiscard]] u32 height() const noexcept override { return m_height; }
  [[nodiscard]] f64 frame_rate() const noexcept override { return m_fps; }
  [[nodiscard]] bool seek(const f64 target_seconds,
                          const bool looping) override {
    const f64 target = nx::max(target_seconds, 0.0);
    const f64 duration = m_playback.duration();
    const f64 local = looping && duration > 0.0
                          ? std::fmod(target, duration)
                          : target;
    if (!m_playback.seek(local, target))
      return false;
    m_finished = false;
    return true;
  }
  [[nodiscard]] f64 position() const noexcept override { return m_position; }
  [[nodiscard]] bool finished() const noexcept override { return m_finished; }

  [[nodiscard]] GpuFrame frame_at(const f64 target_seconds,
                                  const bool looping) override {
    m_playback.set_looping(looping);
    const VideoFrame *const frame = m_playback.advance_to(target_seconds);
    m_finished = m_playback.finished();
    if (frame == nullptr)
      return m_frame;
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
    return m_frame;
  }

private:
  [[nodiscard]] bool upload(const VideoFrame &frame) {
    if (!ensure_textures(frame))
      return false;
    const bool luma = upload_plane(m_luma, frame.y.data(), frame.y.size(),
                                   frame.width, frame.height, frame.width);
    interleave_chroma(frame, m_chroma_scratch);
    if (m_chroma_scratch.empty())
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
    m_luma = make_texture(frame.width, frame.height, nxe::rhi::Format::R8_UNORM,
                          "video.cpu.luma");
    m_chroma = make_texture(frame.chroma_width(), frame.chroma_height(),
                            nxe::rhi::Format::RG8_UNORM, "video.cpu.chroma");
    if (!m_luma.valid() || !m_chroma.valid()) {
      destroy_textures();
      return false;
    }
    m_tex_width = frame.width;
    m_tex_height = frame.height;
    return true;
  }

  [[nodiscard]] nxe::rhi::TextureHandle
  make_texture(const u32 width, const u32 height, const nxe::rhi::Format format,
               const nx::string_view name) const {
    nxe::rhi::TextureDesc desc{};
    desc.name = name;
    desc.format = format;
    desc.width = nx::max(width, 1u);
    desc.height = nx::max(height, 1u);
    desc.usage =
        nxe::rhi::TextureUsage::Sampled | nxe::rhi::TextureUsage::CopyDst;
    return m_device->create_texture(desc);
  }

  [[nodiscard]] bool upload_plane(const nxe::rhi::TextureHandle tex,
                                  const u8 *const data, const usize size,
                                  const u32 width, const u32 height,
                                  const u32 row_pitch) const {
    if (data == nullptr || size == 0 || !tex.valid())
      return false;
    nxe::rhi::ImageSubresource sub{};
    sub.offset = 0;
    sub.size = nx::cast<u64>(size);
    sub.width = width;
    sub.height = height;
    sub.depth = 1;
    sub.row_pitch = row_pitch;
    const nxe::rhi::ImageSubresource layout[1] = {sub};
    return m_device->uploader()
        .upload_texture(tex, std::span<const u8>(data, size),
                        std::span<const nxe::rhi::ImageSubresource>(layout, 1))
        .ok();
  }

  void destroy_textures() noexcept {
    if (m_device != nullptr) {
      if (m_luma.valid())
        m_device->destroy_texture(m_luma);
      if (m_chroma.valid())
        m_device->destroy_texture(m_chroma);
    }
    m_luma = {};
    m_chroma = {};
    m_tex_width = 0;
    m_tex_height = 0;
    m_frame = {};
    m_uploaded_pts = -1.0;
  }

  nxe::rhi::Device *m_device = nullptr;
  PacedPlayback m_playback;
  nxe::rhi::TextureHandle m_luma;
  nxe::rhi::TextureHandle m_chroma;
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

}

GpuSourcePtr create_video_source(nxe::rhi::Device &device,
                                 const nx::string_view path) {
  // The hardware block where it can take the clip (VP9 only, for now), the CPU
  // decoder otherwise. A hardware open that fails still falls through to CPU.
  if (hw_decode_available(device) && webm_is_vp9(path)) {
    auto hw = std::make_unique<HwVideoSource>();
    if (hw->open(device, path))
      return hw;
  }
  auto cpu = std::make_unique<CpuVideoSource>();
  if (cpu->open(device, path))
    return cpu;
  return nullptr;
}

}
