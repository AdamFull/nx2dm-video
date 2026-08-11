# Tests for the video module. Two concerns, one binary:
#  - the shader converts YCbCr and is not upside down (needs a device);
#  - the decoder reads a real .webm (needs libvpx + libwebm, not the engine).
# The decode sources are compiled in directly rather than linking the module, so
# the decode cases pull no RHI and no Engine.

add_executable(nx_video_tests
        test_video_shader.cpp
        test_video_decode.cpp
        ../video_source.cpp
        ../video_decode.cpp)

target_include_directories(nx_video_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/engine" "${CMAKE_SOURCE_DIR}/modules")

target_link_libraries(nx_video_tests
        PRIVATE nx::test nx::foundation nx::rhi nx::vpx nx::webm
        nx::compile_options)

if (ANDROID)
    set(NX_TEST_SHADERS "${NX_ANDROID_SHADER_OUT}")
else ()
    set(NX_TEST_SHADERS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders")
endif ()
target_compile_definitions(nx_video_tests PRIVATE
        NX_TEST_SHADER_DIR="${NX_TEST_SHADERS}"
        NX_VIDEO_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# The cases load the module's own shader, which nx_module() compiles into a
# target of its own.
if (TARGET nx_module_video_shaders)
    add_dependencies(nx_video_tests nx_module_video_shaders)
endif ()

set_target_properties(nx_video_tests PROPERTIES FOLDER "tests")
nxtest_discover_tests(nx_video_tests LABELS video)
