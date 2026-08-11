#include "video/video_source.h"

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

  // A luma ramp that slides diagonally with time, in limited range [16, 235].
  const u32 t = nx::cast<u32>(m_frame * 4);
  for (u32 yy = 0; yy < h; ++yy)
    for (u32 xx = 0; xx < w; ++xx)
      out.y[nx::cast<usize>(yy) * w + xx] =
          nx::cast<u8>(16 + ((xx + yy + t) % 220));

  // A static chroma field: Cb along x, Cr along y. Cr rising downward makes the
  // bottom of the image red, which is the orientation check.
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
  m_clock = 0.0;
  m_has_current = false;
  m_has_next = false;
  m_finished = false;
}

bool PacedPlayback::prime() {
  if (!m_source->next(m_current)) {
    m_finished = true;
    return false;
  }
  m_has_current = true;
  // Do not run the clock ahead of the first frame: a source whose first PTS is
  // not zero should still show frame one before anything is dropped.
  if (m_clock < m_current.pts)
    m_clock = m_current.pts;
  m_has_next = m_source->next(m_next);
  return true;
}

const VideoFrame *PacedPlayback::advance(const f64 dt) {
  if (m_source == nullptr)
    return nullptr;

  m_clock += dt;
  if (!m_has_current && !prime())
    return nullptr;

  for (;;) {
    // Catch up to the clock, dropping every frame it has already passed.
    while (m_has_next && m_next.pts <= m_clock) {
      std::swap(m_current, m_next);
      m_has_next = m_source->next(m_next);
    }
    if (m_has_next)
      break; // the next frame is in hand and not yet due

    // Decode reached the end of the stream.
    if (!m_looping) {
      m_finished = true; // hold the last frame
      break;
    }
    // Loop: restart the timeline from the opening keyframe. The clock resets, so
    // the wrap costs at most one frame of drift, which no one sees.
    m_source->restart();
    m_clock = 0.0;
    m_has_current = false;
    if (!prime())
      return nullptr;
  }

  return m_has_current ? &m_current : nullptr;
}

} // namespace nxm::video
