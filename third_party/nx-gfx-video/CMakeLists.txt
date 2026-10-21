cmake_minimum_required(VERSION 3.21)
project(nx_gfx_video LANGUAGES C CXX)

# nx-gfx-video: hardware video decode over nx-gfx. Vulkan Video (VP9) today;
# on Apple, where nx-gfx runs on Metal, the target is empty for now.
option(NX_GFX_VIDEO_TESTS "Build nx_gfx_video_tests (on nx-foundation's nxtest)" ${PROJECT_IS_TOP_LEVEL})

if (PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
endif ()

# nx-gfx is the consumer's when it has one; built on its own, nx-gfx-video
# fetches the commit it was tested with (FETCHCONTENT_SOURCE_DIR_NX_GFX points
# that at a local checkout).
if (NOT TARGET nx::gfx)
    include(FetchContent)
    set(NX_GFX_TESTS OFF)
    set(NX_FOUNDATION_TESTS ${NX_GFX_VIDEO_TESTS})
    FetchContent_Declare(nx_gfx
            GIT_REPOSITORY https://github.com/AdamFull/nx-gfx.git
            GIT_TAG 140b82051e4553adf9b88651092982e34bbd9a6d)
    FetchContent_MakeAvailable(nx_gfx)
endif ()

set(NX_GFX_VIDEO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(NX_GFX_VIDEO_HEADERS
        include/nx/gfx/video/client.h
        include/nx/gfx/video/decoder.h
        include/nx/gfx/video/vp9.h)

if (APPLE)
    add_library(nx_gfx_video INTERFACE)
    target_link_libraries(nx_gfx_video INTERFACE nx::gfx)
else ()
    add_library(nx_gfx_video STATIC
            ${NX_GFX_VIDEO_HEADERS}
            src/vk_video.cpp
            src/vk_video_client.cpp
            src/vk_vp9.cpp)
    target_include_directories(nx_gfx_video PUBLIC "${NX_GFX_VIDEO_ROOT}/include")
    target_link_libraries(nx_gfx_video
            PUBLIC nx::gfx
            PRIVATE nx::foundation_options)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set_source_files_properties(src/vk_video.cpp src/vk_vp9.cpp
                PROPERTIES COMPILE_OPTIONS "-Wno-missing-field-initializers")
    endif ()
    set_target_properties(nx_gfx_video PROPERTIES FOLDER "nx-gfx-video")
endif ()
add_library(nx::gfx_video ALIAS nx_gfx_video)

if (NX_GFX_VIDEO_TESTS AND NOT APPLE)
    add_subdirectory(tests)
endif ()
