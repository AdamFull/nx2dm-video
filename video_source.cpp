#include "video/video_source.h"

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

} // namespace nxm::video
