#pragma once

#include "video/video_source.h"

#include "nx/gfx/gfx.h"

#include "core/foundation/strings/utf8_string_view.h"

#include <glm/vec2.hpp>
#include <memory>

namespace nx {
class thread_pool;
}

namespace nxm::video {

class WebmVideoDemux;

struct GpuFrame {
  nx::gfx::texture_handle luma;
  nx::gfx::texture_handle chroma;
  glm::vec2 uv_scale{1.f, 1.f};
  ColourInfo colour;
  [[nodiscard]] bool valid() const noexcept {
    return luma.valid() && chroma.valid();
  }
};

class GpuVideoSource {
public:
  virtual ~GpuVideoSource() = default;

  [[nodiscard]] virtual bool valid() const noexcept = 0;
  [[nodiscard]] virtual u32 width() const noexcept = 0;
  [[nodiscard]] virtual u32 height() const noexcept = 0;
  [[nodiscard]] virtual f64 frame_rate() const noexcept = 0;

  [[nodiscard]] virtual bool seek(f64 target_seconds, bool looping) = 0;

  [[nodiscard]] virtual GpuFrame frame_at(f64 target_seconds,
                                          bool looping) = 0;

  [[nodiscard]] virtual f64 position() const noexcept = 0;
  [[nodiscard]] virtual bool finished() const noexcept = 0;
};

using GpuSourcePtr = std::unique_ptr<GpuVideoSource>;

/// A clip read and demuxed with no GPU work: its VP9 track for the hardware
/// decoder, or a source a CPU decoder decodes. Empty when it did not open.
struct OpenedVideo {
  OpenedVideo() noexcept;
  ~OpenedVideo();
  OpenedVideo(OpenedVideo &&) noexcept;
  OpenedVideo &operator=(OpenedVideo &&) noexcept;

  std::unique_ptr<WebmVideoDemux> vp9;
  SourcePtr cpu;
};

[[nodiscard]] bool hw_decode_available(nx::gfx::device &device);

/// Reads and demuxes @p path, for the hardware decoder when @p hardware and
/// the clip is VP9, else for a CPU decoder; an empty path is a synthetic
/// clip. It touches no GPU state, so any thread may open.
[[nodiscard]] OpenedVideo open_video(nx::string_view path, bool hardware);

/// The GPU half of opening a clip, on the render job. Null when @p opened is
/// empty, or the hardware decoder would not take its VP9 track. A CPU-decoded
/// clip decodes on @p workers, a frame behind, when there are any.
[[nodiscard]] GpuSourcePtr
create_video_source(nx::gfx::device &device, OpenedVideo opened,
                    nx::thread_pool *workers = nullptr);
}
