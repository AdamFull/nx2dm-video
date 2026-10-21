# nx gfx video

Hardware video decode over [nx-gfx](https://github.com/AdamFull/nx-gfx): Vulkan
Video sessions that decode into images nx-gfx samples, without the frames
leaving the GPU. VP9 today; its Metal and VideoToolbox counterpart is later
work, and on Apple the `nx::gfx_video` target is empty for now.

## Using it

Add nx-foundation and nx-gfx, then nx-gfx-video, and link `nx::gfx_video`:

```cmake
add_subdirectory(external/nx-foundation)
add_subdirectory(external/nx-gfx)
add_subdirectory(external/nx-gfx-video)
target_link_libraries(my_player PRIVATE nx::gfx_video)
```

Built on its own, it fetches nx-gfx (and through it nx-foundation) at the
commits it was tested with; `FETCHCONTENT_SOURCE_DIR_NX_GFX` points that at a
checkout.

A device that should decode is made with the decode client, which asks the
Vulkan device for the video extensions, the VP9 feature and a queue in a decode
family of its own:

```cpp
#include "nx/gfx/video/client.h"
#include "nx/gfx/video/decoder.h"

nx::gfx::native_device_client *clients[] = {&nx::gfx::video::video_decode_client()};
auto made = nx::gfx::create_device({.application_name = "player",
                                    .native_clients = clients});

nx::gfx::video::video_decoder decoder;
if (decoder.init(*made.value(), nx::gfx::video::video_codec::VP9, width, height)) {
  bool shown = false;
  u32 w = 0, h = 0;
  if (decoder.decode_frame(frame, size, shown, w, h) && shown) {
    // sample decoder.luma_texture() and decoder.chroma_texture()
  }
}
```

`query_decode_caps` reports what a device can decode without making a session;
`parse_vp9_frame_header` (`nx/gfx/video/vp9.h`) reads the uncompressed VP9
frame header the session needs.

## Layout

| Path | What |
| --- | --- |
| `include/nx/gfx/video/` | `decoder.h`, `client.h`, `vp9.h` |
| `src/` | the Vulkan Video implementation |
| `tests/` | `nx_gfx_video_tests`, on nx-foundation's nxtest |

## Tests

`NX_GFX_VIDEO_TESTS` (on when built on its own) builds `nx_gfx_video_tests`,
registered with ctest under the label `gfx_video`. They skip where no device
can be made, and the session test where the device has no decode queue.
