
option(NX_MODULE_VIDEO "Build WebM video playback" OFF)

include("${CMAKE_CURRENT_SOURCE_DIR}/NxVideo.cmake")

# The host asset compiler supports installed module formats independently of
# whether a particular target enables the runtime module. Keep this lightweight
# path ahead of the option gate; target builds return inside nx_asset_cooker().
if (NX_BUILD_TOOLS AND TARGET assetc AND NOT ANDROID AND NOT IOS)
    nx_add_video_container()
endif ()
nx_asset_cooker(video
        SOURCES video_cooker.cpp video_asset.cpp video_clip.cpp video_webm.cpp
        DEPENDS nx::webm)

if (NOT NX_MODULE_VIDEO)
    return()
endif ()

nx_add_video_codecs()

nx_module(video
        DISPLAY "WebM video playback"
        SOURCES video_module.cpp video_source.cpp video_pass.cpp video_decode.cpp
                video_demux.cpp video_mediacodec.cpp video_clip.cpp video_asset.cpp
                video_webm.cpp
                video_audio.cpp video_scripting.cpp video_hw.cpp video_cpu.cpp
        DEPENDS nx::vpx nx::webm nx::opus nx::vorbis nx::gav1
        ASSETS assets)

if (ANDROID)
    target_link_libraries(nx_module_video PUBLIC mediandk log)
    set_source_files_properties(video_mediacodec.cpp PROPERTIES
            COMPILE_DEFINITIONS __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__)
endif ()

if (NX_BUILD_TESTS)
    add_subdirectory(tests)
endif ()
