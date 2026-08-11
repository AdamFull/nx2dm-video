# Tests for the video module's shader: that it converts YCbCr to RGB and is not
# upside down. They need a device to render into and read the pixels back, so
# they link nx::rhi; the shader under test is the one nx_module() compiles.

add_executable(nx_video_tests test_video_shader.cpp)

target_link_libraries(nx_video_tests
        PRIVATE nx::test nx::foundation nx::rhi nx::compile_options)

if (ANDROID)
    set(NX_TEST_SHADERS "${NX_ANDROID_SHADER_OUT}")
else ()
    set(NX_TEST_SHADERS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders")
endif ()
target_compile_definitions(nx_video_tests PRIVATE
        NX_TEST_SHADER_DIR="${NX_TEST_SHADERS}")

# The cases load the module's own shader, which nx_module() compiles into a
# target of its own.
if (TARGET nx_module_video_shaders)
    add_dependencies(nx_video_tests nx_module_video_shaders)
endif ()

set_target_properties(nx_video_tests PROPERTIES FOLDER "tests")
nxtest_discover_tests(nx_video_tests LABELS video)
