#include "video/video_source.h"

#include <cmath>
#include <utility>

namespace nxm::video {

SyntheticSource::SyntheticSource(const u32 width, const u32 height,
                                 const f64 fps) noexcept
    : m_width(width == 0 ? 2 : width), m_height(height == 0 ? 2 : height),
      m_fps(fps > 0.0 ? fps : 30.0) {}

bool SyntheticSource::next(VideoFrame &out) {
  const u32 w = m_width;
  const u32 h = m_height;
  const u32 cw = (w + 1) / 2;
  const u32 ch = (h + 1) / 2;

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
}

void PacedPlayback::reset(SourcePtr source, const bool looping) {
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
  if (m_source == nullptr)
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
  if (m_source == nullptr)
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

  m_presentation_clock += dt;
  m_clock += dt;

  const auto avail = [&] { return budget == nullptr || *budget > 0; };
  const auto spend = [&] {
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
      if (m_source->next(m_next))
        m_has_next = true;
      else
        m_eos = true;
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

}
