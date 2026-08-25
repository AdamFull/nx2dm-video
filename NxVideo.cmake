
set(NX_LIBVPX_REPOSITORY "https://github.com/webmproject/libvpx.git"
        CACHE STRING "Where to fetch libvpx from")
set(NX_LIBVPX_TAG "9cc8e1c18024d6b64422ecb7fdd7a43c8e873908"
        CACHE STRING "libvpx commit to build against (v1.16.0)")
set(NX_LIBVPX_SOURCE_DIR "" CACHE PATH
        "An existing libvpx checkout. Empty fetches NX_LIBVPX_TAG.")

set(NX_LIBWEBM_REPOSITORY "https://github.com/webmproject/libwebm.git"
        CACHE STRING "Where to fetch libwebm from")
set(NX_LIBWEBM_TAG "6184f4484a826724b5293837134ab9492261b941"
        CACHE STRING "libwebm commit to build against")
set(NX_LIBWEBM_SOURCE_DIR "" CACHE PATH
        "An existing libwebm checkout. Empty fetches NX_LIBWEBM_TAG.")

set(NX_LIBGAV1_REPOSITORY "https://chromium.googlesource.com/codecs/libgav1"
        CACHE STRING "Where to fetch libgav1 from")
set(NX_LIBGAV1_TAG "c1deec657b32b911920c78e078cfd089faa77200"
        CACHE STRING "libgav1 commit to build against")
set(NX_LIBGAV1_SOURCE_DIR "" CACHE PATH
        "An existing libgav1 checkout. Empty fetches NX_LIBGAV1_TAG.")

function(nx_add_video_container)
    if (TARGET nx_webm)
        return()
    endif ()

    include(FetchContent)

    set(_webm_submodule "${CMAKE_CURRENT_LIST_DIR}/third_party/libwebm")
    if (NX_LIBWEBM_SOURCE_DIR)
        set(_webm_dir "${NX_LIBWEBM_SOURCE_DIR}")
    elseif (EXISTS "${_webm_submodule}/mkvparser/mkvparser.h")
        set(_webm_dir "${_webm_submodule}")
        message(STATUS "nx2d: libwebm from submodule")
    else ()
        FetchContent_Declare(libwebm
                GIT_REPOSITORY "${NX_LIBWEBM_REPOSITORY}"
                GIT_TAG "${NX_LIBWEBM_TAG}"
                GIT_PROGRESS TRUE
                SOURCE_SUBDIR "cmake-is-not-here")
        message(STATUS "nx2d: fetching libwebm ${NX_LIBWEBM_TAG}")
        FetchContent_MakeAvailable(libwebm)
        set(_webm_dir "${libwebm_SOURCE_DIR}")
    endif ()
    if (NOT EXISTS "${_webm_dir}/mkvparser/mkvparser.h")
        message(FATAL_ERROR "nx2d: no libwebm at ${_webm_dir}")
    endif ()

    add_library(nx_webm STATIC
            "${_webm_dir}/mkvparser/mkvparser.cc"
            "${_webm_dir}/mkvparser/mkvreader.cc")
    add_library(nx::webm ALIAS nx_webm)
    target_include_directories(nx_webm SYSTEM PUBLIC "${_webm_dir}")
    set_target_properties(nx_webm PROPERTIES FOLDER "third_party")
endfunction()

function(nx_add_video_codecs)
    if (TARGET nx_vpx)
        return()
    endif ()

    include(FetchContent)
    nx_add_video_container()

    set(_config "${CMAKE_CURRENT_LIST_DIR}/third_party/libvpx_config")

    set(_vpx_submodule "${CMAKE_CURRENT_LIST_DIR}/third_party/libvpx")
    if (NX_LIBVPX_SOURCE_DIR)
        set(_vpx_dir "${NX_LIBVPX_SOURCE_DIR}")
    elseif (EXISTS "${_vpx_submodule}/vpx/vpx_decoder.h")
        set(_vpx_dir "${_vpx_submodule}")
        message(STATUS "nx2d: libvpx from submodule")
    else ()
        FetchContent_Declare(libvpx
                GIT_REPOSITORY "${NX_LIBVPX_REPOSITORY}"
                GIT_TAG "${NX_LIBVPX_TAG}"
                GIT_PROGRESS TRUE
                SOURCE_SUBDIR "cmake-is-not-here")
        message(STATUS "nx2d: fetching libvpx ${NX_LIBVPX_TAG}")
        FetchContent_MakeAvailable(libvpx)
        set(_vpx_dir "${libvpx_SOURCE_DIR}")
    endif ()
    if (NOT EXISTS "${_vpx_dir}/vpx/vpx_decoder.h")
        message(FATAL_ERROR "nx2d: no libvpx at ${_vpx_dir}")
    endif ()

    file(STRINGS "${_config}/vpx_decode_sources.txt" _vpx_rel)
    set(_vpx_srcs "")
    foreach (_rel IN LISTS _vpx_rel)
        if (_rel)
            list(APPEND _vpx_srcs "${_vpx_dir}/${_rel}")
        endif ()
    endforeach ()

    add_library(nx_vpx STATIC ${_vpx_srcs})
    add_library(nx::vpx ALIAS nx_vpx)
    target_include_directories(nx_vpx SYSTEM PUBLIC
            "${_config}/generic" "${_vpx_dir}")
    set_target_properties(nx_vpx PROPERTIES FOLDER "third_party")

    set(LIBGAV1_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LIBGAV1_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(LIBGAV1_THREADPOOL_USE_STD_MUTEX 1 CACHE STRING "" FORCE)

    set(_gav1_submodule "${CMAKE_CURRENT_LIST_DIR}/third_party/libgav1")
    if (NX_LIBGAV1_SOURCE_DIR)
        set(_gav1_dir "${NX_LIBGAV1_SOURCE_DIR}")
    elseif (EXISTS "${_gav1_submodule}/src/gav1/decoder.h")
        set(_gav1_dir "${_gav1_submodule}")
        message(STATUS "nx2d: libgav1 from submodule")
    endif ()

    if (_gav1_dir)
        add_subdirectory("${_gav1_dir}"
                "${CMAKE_BINARY_DIR}/_deps/libgav1-build" EXCLUDE_FROM_ALL)
    else ()
        FetchContent_Declare(libgav1
                GIT_REPOSITORY "${NX_LIBGAV1_REPOSITORY}"
                GIT_TAG "${NX_LIBGAV1_TAG}"
                GIT_PROGRESS TRUE)
        message(STATUS "nx2d: fetching libgav1 ${NX_LIBGAV1_TAG}")
        FetchContent_MakeAvailable(libgav1)
        set(_gav1_dir "${libgav1_SOURCE_DIR}")
    endif ()

    if (NOT TARGET libgav1_static)
        message(FATAL_ERROR "nx2d: libgav1_static was not created")
    endif ()
    add_library(nx::gav1 ALIAS libgav1_static)
    target_include_directories(libgav1_static SYSTEM INTERFACE "${_gav1_dir}/src")
    set_target_properties(libgav1_static PROPERTIES FOLDER "third_party")
endfunction()
