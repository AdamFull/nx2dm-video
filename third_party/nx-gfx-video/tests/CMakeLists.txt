# nx_gfx_video_tests: the library's own suite, on nxtest from nx-foundation.
add_executable(nx_gfx_video_tests
        owned_device.h
        test_decode_caps.cpp)
target_link_libraries(nx_gfx_video_tests
        PRIVATE nx::test nx::gfx_video nx::foundation_options)
set_target_properties(nx_gfx_video_tests PROPERTIES FOLDER "nx-gfx-video")

nxtest_discover_tests(nx_gfx_video_tests LABELS gfx_video TIMEOUT 180)
