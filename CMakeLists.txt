# WebM video playback as a module. See docs/video.md.
#
# The option is declared here rather than left to nx_module() because the codecs
# have to be fetched and built before the module can link them, and a build with
# the module off should not fetch libvpx/libwebm at all. Same reason Live2D
# declares its own.

option(NX_MODULE_VIDEO "Build WebM video playback" OFF)
if (NOT NX_MODULE_VIDEO)
    return()
endif ()

include("${CMAKE_CURRENT_SOURCE_DIR}/NxVideo.cmake")
nx_add_video_codecs()

nx_module(video
        DISPLAY "WebM video playback"
        SOURCES video_module.cpp video_source.cpp video_pass.cpp video_decode.cpp
                video_audio.cpp
        DEPENDS nx::vpx nx::webm nx::opus
        ASSETS assets)

if (NX_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
