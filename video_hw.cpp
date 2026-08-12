#include "video/video_hw.h"

#include "core/foundation/diagnostics/log.h"

namespace nxm::video {

#if defined(NX_RHI_VULKAN)

} // namespace nxm::video

#include "core/rendering/rhi/vulkan/vk_video.h"

#include "video/video_demux.h"

namespace nxm::video {
namespace {

namespace rhi = nxe::rhi;

} // namespace

struct HwVideoSource::Impl {
  rhi::VideoDecoder decoder;
  WebmVideoDemux demux;
  nx::vector<u8> next_bytes;
  f64 next_pts = 0.0;
  bool have_next = false;
  f64 current_pts = -1.0;
  GpuFrame frame;
  bool valid = false;
  bool finished = false;

  bool demux_next() {
    const u8 *data = nullptr;
    long len = 0;
    if (!demux.next(data, len, next_pts))
      return false;
    next_bytes.assign(data, data + len);
    return true;
  }

  void pull_next(const bool looping) {
    if (demux_next()) {
      have_next = true;
      return;
    }
    if (looping) {
      demux.restart();
      if (demux_next()) {
        have_next = true;
        return;
      }
    }
    have_next = false;
  }
};

bool hw_decode_available(rhi::Device &device) {
  return device.video_decode_available();
}

HwVideoSource::HwVideoSource() : m(std::make_unique<Impl>()) {}
HwVideoSource::~HwVideoSource() = default;

bool HwVideoSource::open(rhi::Device &device, const nx::string_view path) {
  if (!m->demux.open(path, "V_VP9")) {
    nx::logw("video: hw demux failed for '{}'", path);
    return false;
  }
  if (!m->decoder.init(device, rhi::VideoCodec::VP9, m->demux.width(),
                       m->demux.height())) {
    nx::logw("video: hw decoder init failed for '{}'", path);
    return false;
  }
  m->pull_next(false);
  m->valid = m->have_next;
  return m->valid;
}

bool HwVideoSource::valid() const noexcept { return m->valid; }
u32 HwVideoSource::width() const noexcept { return m->demux.width(); }
u32 HwVideoSource::height() const noexcept { return m->demux.height(); }
f64 HwVideoSource::frame_rate() const noexcept { return m->demux.frame_rate(); }
f64 HwVideoSource::position() const noexcept { return m->current_pts; }
bool HwVideoSource::finished() const noexcept { return m->finished; }

GpuFrame HwVideoSource::frame_at(const f64 target_seconds, const bool looping) {
  if (target_seconds + 1e-6 < m->current_pts) {
    m->decoder.reset_stream();
    (void)m->demux.seek(target_seconds);
    m->current_pts = -1.0;
    m->finished = false;
    m->pull_next(false);
  }

  bool advanced = false;
  u32 shown_w = 0;
  u32 shown_h = 0;
  // Decode every frame up to the target - VP9 is a reference chain, none can be
  // skipped - and remember the last one, which is the one to show.
  while (m->have_next && m->next_pts <= target_seconds) {
    bool shown = false;
    u32 w = 0;
    u32 h = 0;
    if (m->decoder.decode_frame(m->next_bytes.data(),
                                nx::cast<u32>(m->next_bytes.size()), shown, w,
                                h)) {
      if (shown) {
        advanced = true;
        shown_w = w;
        shown_h = h;
        m->current_pts = m->next_pts;
      }
    }
    m->pull_next(looping);
    if (!m->have_next && !looping)
      m->finished = true;
  }

  if (advanced && m->decoder.show_last()) {
    m->frame.luma = m->decoder.luma_texture();
    m->frame.chroma = m->decoder.chroma_texture();
    m->frame.colour = m->demux.colour();
    const rhi::Extent2D coded = m->decoder.coded_extent();
    m->frame.uv_scale = {
        coded.width != 0 ? nx::cast<f32>(shown_w) / coded.width : 1.f,
        coded.height != 0 ? nx::cast<f32>(shown_h) / coded.height : 1.f};
  }
  return m->frame;
}

#else // no hardware video decode on this backend

bool hw_decode_available(nxe::rhi::Device &) { return false; }

struct HwVideoSource::Impl {};
HwVideoSource::HwVideoSource() = default;
HwVideoSource::~HwVideoSource() = default;
bool HwVideoSource::open(nxe::rhi::Device &, nx::string_view) { return false; }
bool HwVideoSource::valid() const noexcept { return false; }
u32 HwVideoSource::width() const noexcept { return 0; }
u32 HwVideoSource::height() const noexcept { return 0; }
f64 HwVideoSource::frame_rate() const noexcept { return 0.0; }
f64 HwVideoSource::position() const noexcept { return 0.0; }
bool HwVideoSource::finished() const noexcept { return true; }
GpuFrame HwVideoSource::frame_at(f64, bool) { return {}; }

#endif

} // namespace nxm::video
