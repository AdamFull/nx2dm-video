
add_executable(nx_video_tests
        test_video_shader.cpp
        test_video_decode.cpp
        test_video_assets.cpp
        test_video_vp9.cpp
        test_video_place.cpp
        ../video_source.cpp
        ../video_decode.cpp
        ../video_demux.cpp
        ../video_clip.cpp
        ../video_asset.cpp
        ../video_webm.cpp
        ../video_audio.cpp
        ../video_hw.cpp
        ../video_cpu.cpp
        ../video_mediacodec.cpp)

target_include_directories(nx_video_tests PRIVATE
        "${NX_ENGINE_ROOT}/engine" "${NX_ENGINE_ROOT}/modules")

target_link_libraries(nx_video_tests
        PRIVATE nx::test nx::foundation nx::rhi nx::glm nx::vpx nx::webm nx::opus
        nx::vorbis nx::gav1 nx::compile_options)

if (ANDROID)
    set(NX_TEST_SHADERS "${NX_ANDROID_SHADER_OUT}")
else ()
    set(NX_TEST_SHADERS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders")
endif ()
target_compile_definitions(nx_video_tests PRIVATE
        NX_TEST_SHADER_DIR="${NX_TEST_SHADERS}"
        NX_VIDEO_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

if (TARGET nx_module_video_shaders)
    add_dependencies(nx_video_tests nx_module_video_shaders)
endif ()

set_target_properties(nx_video_tests PROPERTIES FOLDER "tests")
nxtest_discover_tests(nx_video_tests LABELS video)

# Compile the asset resolution boundary with the actual Shipping policy. The
# normal test binary deliberately keeps authored fallback enabled, so a second
# focused binary is necessary to prove that a sealed runtime cannot open raw
# descriptors or media.
add_executable(nx_video_shipping_asset_tests
        test_video_assets_shipping.cpp
        ../video_demux.cpp
        ../video_clip.cpp
        ../video_asset.cpp
        ../video_webm.cpp)
target_include_directories(nx_video_shipping_asset_tests PRIVATE
        "${NX_ENGINE_ROOT}/engine" "${NX_ENGINE_ROOT}/modules")
target_link_libraries(nx_video_shipping_asset_tests
        PRIVATE nx::test nx::foundation nx::webm nx::compile_options)
target_compile_definitions(nx_video_shipping_asset_tests PRIVATE
        NX_BUILD_SHIPPING=1
        NX_VIDEO_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
set_target_properties(nx_video_shipping_asset_tests PROPERTIES FOLDER "tests")
nxtest_discover_tests(nx_video_shipping_asset_tests
        PREFIX "shipping: " LABELS video)

add_executable(nx_video_scripting_tests
        test_video_scripting.cpp ../video_scripting.cpp)
target_include_directories(nx_video_scripting_tests PRIVATE
        "${NX_ENGINE_ROOT}/engine" "${NX_ENGINE_ROOT}/modules")
target_link_libraries(nx_video_scripting_tests
        PRIVATE nx::test nx::foundation nx::engine nx::glm nx::compile_options)
set_target_properties(nx_video_scripting_tests PROPERTIES FOLDER "tests")
nxtest_discover_tests(nx_video_scripting_tests LABELS video)

target_compile_definitions(nx_video_scripting_tests PRIVATE
        NX_MODULE_SERVICES_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/../script-services.json")
target_link_libraries(nx_video_scripting_tests PRIVATE nx::script_luau)
