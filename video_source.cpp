#include "video/video_source.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace nxm::video {

namespace {

[[nodiscard]] bool plane_fits(const usize size, const u32 pitch,
                              const u32 width, const u32 height) noexcept {
  if (width == 0 || height == 0 || pitch < width)
    return false;
  const u64 required = nx::cast<u64>(height - 1) * pitch + width;
  return required <= size;
}

[[nodiscard]] bool copy_plane(u8 *const dst, const u32 dst_pitch,
                              const u8 *const src, const ptrdiff_t src_pitch,
                              const u32 width, const u32 height) noexcept {
  if (dst == nullptr || src == nullptr || width == 0 || height == 0 ||
      dst_pitch < width || src_pitch == std::numeric_limits<ptrdiff_t>::min())
    return false;
  const usize magnitude =
      nx::cast<usize>(src_pitch < 0 ? -src_pitch : src_pitch);
  if (magnitude < width)
    return false;
  for (u32 row = 0; row < height; ++row)
    std::memcpy(dst + nx::cast<usize>(row) * dst_pitch,
                src + nx::cast<ptrdiff_t>(row) * src_pitch, width);
  return true;
}

[[nodiscard]] f64 bounded_clock_add(const f64 clock,
                                    const f64 elapsed) noexcept {
  constexpr f64 LARGEST = std::numeric_limits<f64>::max();
  return elapsed > LARGEST - clock ? LARGEST : clock + elapsed;
}

} // namespace

bool valid_video_dimensions(const u64 width, const u64 height) noexcept {
  if (width == 0 || height == 0 || width > MAX_VIDEO_DIMENSION ||
      height > MAX_VIDEO_DIMENSION)
    return false;
  const u64 luma = width * height;
  const u64 chroma_width = width / 2 + width % 2;
  const u64 chroma_height = height / 2 + height % 2;
  const u64 chroma = chroma_width * chroma_height;
  return luma <= MAX_DECODED_VIDEO_FRAME_BYTES &&
         chroma <= (MAX_DECODED_VIDEO_FRAME_BYTES - luma) / 2;
}

bool valid_video_frame(const VideoFrame &frame) noexcept {
  if (!valid_video_dimensions(frame.width, frame.height) ||
      !std::isfinite(frame.pts))
    return false;
  const u32 cw = frame.chroma_width();
  const u32 ch = frame.chroma_height();
  return plane_fits(frame.y.size(), frame.y_pitch, frame.width, frame.height) &&
         plane_fits(frame.cb.size(), frame.c_pitch, cw, ch) &&
         plane_fits(frame.cr.size(), frame.c_pitch, cw, ch);
}

bool fill_i420(VideoFrame &out, const u32 width, const u32 height,
               const u32 chroma_width, const u32 chroma_height, const f64 pts,
               const u8 *const y, const ptrdiff_t y_stride, const u8 *const cb,
               const ptrdiff_t cb_stride, const u8 *const cr,
               const ptrdiff_t cr_stride) noexcept {
  if (!valid_video_dimensions(width, height) ||
      chroma_width != width / 2 + width % 2 ||
      chroma_height != height / 2 + height % 2 || !std::isfinite(pts))
    return false;

  try {
    VideoFrame decoded;
    decoded.width = width;
    decoded.height = height;
    decoded.y_pitch = width;
    decoded.c_pitch = chroma_width;
    decoded.pts = nx::max(pts, 0.0);
    decoded.y.resize(nx::cast<usize>(width) * height);
    decoded.cb.resize(nx::cast<usize>(chroma_width) * chroma_height);
    decoded.cr.resize(nx::cast<usize>(chroma_width) * chroma_height);
    if (!copy_plane(decoded.y.data(), width, y, y_stride, width, height) ||
        !copy_plane(decoded.cb.data(), chroma_width, cb, cb_stride,
                    chroma_width, chroma_height) ||
        !copy_plane(decoded.cr.data(), chroma_width, cr, cr_stride,
                    chroma_width, chroma_height))
      return false;
    out = std::move(decoded);
    return true;
  } catch (...) {
    return false;
  }
}

bool interleave_chroma(const VideoFrame &frame, nx::vector<u8> &out) noexcept {
  if (!valid_video_dimensions(frame.width, frame.height)) {
    out.clear();
    return false;
  }
  const usize texels =
      nx::cast<usize>(frame.chroma_width()) * frame.chroma_height();
  if (frame.cb.size() < texels || frame.cr.size() < texels) {
    out.clear();
    return false;
  }
  try {
    out.resize(texels * 2);
    for (usize i = 0; i < texels; ++i) {
      out[2 * i] = frame.cb[i];
      out[2 * i + 1] = frame.cr[i];
    }
    return true;
  } catch (...) {
    out.clear();
    return false;
  }
}

SyntheticSource::SyntheticSource(const u32 width, const u32 height,
                                 const f64 fps) noexcept
    : m_width(valid_video_dimensions(width, height) ? width : 2),
      m_height(valid_video_dimensions(width, height) ? height : 2),
      m_fps(std::isfinite(fps) && fps > 0.0 && fps <= MAX_VIDEO_FRAME_RATE
                ? fps
                : 30.0) {}

bool SyntheticSource::next(VideoFrame &out) {
  const u32 w = m_width;
  const u32 h = m_height;
  const u32 cw = (w + 1) / 2;
  const u32 ch = (h + 1) / 2;

  try {
    out.width = w;
    out.height = h;
    out.y_pitch = w;
    out.c_pitch = cw;
    out.pts = nx::cast<f64>(m_frame) / m_fps;
    out.y.resize(nx::cast<usize>(w) * h);
    out.cb.resize(nx::cast<usize>(cw) * ch);
    out.cr.resize(nx::cast<usize>(cw) * ch);

    const u32 t = nx::cast<u32>(m_frame * 4);
    for (u32 yy = 0; yy < h; ++yy)
      for (u32 xx = 0; xx < w; ++xx)
        out.y[nx::cast<usize>(yy) * w + xx] =
            nx::cast<u8>(16 + ((xx + yy + t) % 220));

    for (u32 yy = 0; yy < ch; ++yy)
      for (u32 xx = 0; xx < cw; ++xx) {
        out.cb[nx::cast<usize>(yy) * cw + xx] =
            nx::cast<u8>(cw > 1 ? xx * 255u / (cw - 1) : 128u);
        out.cr[nx::cast<usize>(yy) * cw + xx] =
            nx::cast<u8>(ch > 1 ? yy * 255u / (ch - 1) : 128u);
      }

    ++m_frame;
    return true;
  } catch (...) {
    out = {};
    return false;
  }
}

void PacedPlayback::reset(SourcePtr source, const bool looping) {
  if (source != nullptr &&
      (!valid_video_dimensions(source->width(), source->height()) ||
       !std::isfinite(source->frame_rate()) || source->frame_rate() <= 0.0 ||
       source->frame_rate() > MAX_VIDEO_FRAME_RATE))
    source.reset();
  m_source = std::move(source);
  m_looping = looping;
  m_presentation_clock = 0.0;
  m_clock = 0.0;
  m_has_current = false;
  m_has_next = false;
  m_finished = false;
  m_eos = false;
}

bool PacedPlayback::seek(const f64 target_seconds) {
  return seek(target_seconds, target_seconds);
}

bool PacedPlayback::seek(const f64 source_seconds,
                         const f64 presentation_seconds) {
  if (m_source == nullptr || !std::isfinite(source_seconds) ||
      !std::isfinite(presentation_seconds))
    return false;
  const f64 source = source_seconds > 0.0 ? source_seconds : 0.0;
  const f64 presentation =
      presentation_seconds > 0.0 ? presentation_seconds : 0.0;
  if (!m_source->seek(source))
    return false;
  m_presentation_clock = presentation;
  m_clock = source;
  m_has_current = false;
  m_has_next = false;
  m_eos = false;
  m_finished = false;
  return true;
}

const VideoFrame *PacedPlayback::advance_to(const f64 target,
                                            i32 *const budget) {
  if (m_source == nullptr || !std::isfinite(target))
    return nullptr;
  if (target + 1e-6 < m_presentation_clock && !seek(target)) {
    // Seeking is optional on FrameSource. A backward presentation time must
    // still rewind sources which only implement the mandatory restart().
    m_source->restart();
    m_presentation_clock = 0.0;
    m_clock = 0.0;
    m_has_current = false;
    m_has_next = false;
    m_eos = false;
    m_finished = false;
  }
  const f64 dt = target - m_presentation_clock;
  return advance(dt > 0.0 ? dt : 0.0, budget);
}

const VideoFrame *PacedPlayback::advance(const f64 dt, i32 *const budget) {
  if (m_source == nullptr)
    return nullptr;

  const f64 elapsed = std::isfinite(dt) && dt > 0.0 ? dt : 0.0;
  m_presentation_clock = bounded_clock_add(m_presentation_clock, elapsed);
  m_clock = bounded_clock_add(m_clock, elapsed);

  i32 steps = MAX_VIDEO_DECODE_STEPS_PER_ADVANCE;
  const auto avail = [&] {
    return steps > 0 && (budget == nullptr || *budget > 0);
  };
  const auto spend = [&] {
    --steps;
    if (budget != nullptr && *budget > 0)
      --*budget;
  };

  for (;;) {
    if (!m_has_current) {
      if (!avail())
        return nullptr;
      spend();
      if (!m_source->next(m_current)) {
        m_eos = true;
        m_finished = true;
        return nullptr;
      }
      if (!valid_video_frame(m_current)) {
        m_current = {};
        m_eos = true;
        m_finished = true;
        return nullptr;
      }
      m_has_current = true;
      // Do not run the clock ahead of the first frame: a non-zero opening PTS
      // should still show frame one before anything is dropped.
      if (m_clock < m_current.pts)
        m_clock = m_current.pts;
      continue;
    }

    if (!m_has_next && !m_eos) {
      if (!avail())
        break;
      spend();
      if (m_source->next(m_next)) {
        if (!valid_video_frame(m_next)) {
          m_next = {};
          m_eos = true;
        } else {
          m_has_next = true;
        }
      } else {
        m_eos = true;
      }
    }

    if (m_has_next && m_next.pts <= m_clock) {
      std::swap(m_current, m_next);
      m_has_next = false;
      continue;
    }

    if (m_has_next || !m_eos)
      break;

    if (!m_looping) {
      m_finished = true;
      break;
    }
    const f64 frame_time = 1.0 / nx::max(m_source->frame_rate(), 1.0);
    const f64 cycle = m_current.pts + frame_time;
    if (m_clock + 1e-9 < cycle)
      break;

    m_clock = cycle > 0.0 ? std::fmod(m_clock, cycle) : 0.0;
    m_source->restart();
    m_has_current = false;
    m_has_next = false;
    m_eos = false;
  }

  return m_has_current ? &m_current : nullptr;
}

} // namespace nxm::video
