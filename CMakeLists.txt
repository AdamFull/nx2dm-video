
option(NX_MODULE_VIDEO "Build WebM video playback" OFF)
if (NOT NX_MODULE_VIDEO)
    return()
endif ()

include("${CMAKE_CURRENT_SOURCE_DIR}/NxVideo.cmake")
nx_add_video_codecs()

nx_module(video
        DISPLAY "WebM video playback"
        SOURCES video_module.cpp video_source.cpp video_pass.cpp video_decode.cpp
                video_demux.cpp video_mediacodec.cpp video_clip.cpp
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
