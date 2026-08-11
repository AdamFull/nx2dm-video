# WebM video playback as a module. See docs/video.md.
#
# V1 is the engine-side spine: a synthetic YUV source drives decode -> upload ->
# YCbCr convert -> draw with no codec yet, so the whole path is testable before
# libwebm/libvpx land. No third-party dependency at this stage.

nx_module(video
        DISPLAY "WebM video playback"
        DEFAULT OFF
        SOURCES video_module.cpp video_source.cpp video_pass.cpp
        ASSETS assets)

if (NX_MODULE_VIDEO AND NX_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
