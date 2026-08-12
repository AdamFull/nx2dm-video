# libvpx (VP9 decode) and libwebm (WebM demux), the two runtime codecs the
# video module needs. Vendored as module-local submodules under third_party/,
# pinned to the commits below. A checkout that skipped --recursive, or that
# wants a different revision, falls back to fetching on demand - so a plain
# clone still builds the module without a submodule step.
#
# libvpx has no CMake build - it is a POSIX-shell configure plus a Perl run-time-
# CPU-detect generator. We do not run either per build. Instead the generated
# config for the pure-C `generic-gnu` target is committed under
# third_party/libvpx_config/generic, and that config is arch-independent for
# every little-endian target we ship (x86-64, arm64): all VPX_ARCH_* and HAVE_*
# are 0, so libvpx takes its plain C paths everywhere. SIMD is a later, per-arch
# optimisation; correctness does not wait on it. The decode-only source list is
# committed alongside as vpx_decode_sources.txt - what a --enable-vp9-decoder
# --disable-everything-else configure resolves to, minus the encoder DSP files
# gated by ifeq(CONFIG_ENCODERS) that a decoder never links.

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

# libgav1 (AV1 decode). Unlike libvpx it has a real CMake build, so we run it
# rather than hand-committing a config. Configured decode-only with the
# std::mutex thread pool, which drops its one external dependency (Abseil), and
# with no examples or tests.
set(NX_LIBGAV1_REPOSITORY "https://chromium.googlesource.com/codecs/libgav1"
        CACHE STRING "Where to fetch libgav1 from")
set(NX_LIBGAV1_TAG "c1deec657b32b911920c78e078cfd089faa77200"
        CACHE STRING "libgav1 commit to build against")
set(NX_LIBGAV1_SOURCE_DIR "" CACHE PATH
        "An existing libgav1 checkout. Empty fetches NX_LIBGAV1_TAG.")

function(nx_add_video_codecs)
    include(FetchContent)

    set(_config "${CMAKE_CURRENT_LIST_DIR}/third_party/libvpx_config")

    # libvpx: an explicit override wins, then the checked-out submodule, then a
    # fetch for a clone that skipped --recursive.
    set(_vpx_submodule "${CMAKE_CURRENT_LIST_DIR}/third_party/libvpx")
    if (NX_LIBVPX_SOURCE_DIR)
        set(_vpx_dir "${NX_LIBVPX_SOURCE_DIR}")
    elseif (EXISTS "${_vpx_submodule}/vpx/vpx_decoder.h")
        set(_vpx_dir "${_vpx_submodule}")
        message(STATUS "nx2d: libvpx from submodule")
    else ()
        # A full clone rather than shallow: a commit is not a ref a shallow
        # fetch can name, and pinning a commit is what keeps the decoder stable
        # under us between two builds of one nx2d revision.
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
    # The committed generic config first, then the source root: libvpx's files
    # include "./vpx_config.h" and "./vpx_dsp_rtcd.h", found on the include path.
    # SYSTEM so libvpx's own warnings never fail an engine build.
    target_include_directories(nx_vpx SYSTEM PUBLIC
            "${_config}/generic" "${_vpx_dir}")
    set_target_properties(nx_vpx PROPERTIES FOLDER "third_party")

    # libwebm - the mkvparser only; the muxer and the sample apps are not built.
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

    # libgav1 - built through its own CMake. The options must be set before the
    # subdirectory runs; std::mutex threading is what avoids Abseil.
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
    # Consumers include "gav1/decoder.h"; make sure the header root is on the
    # path, and SYSTEM so libgav1's headers never warn into an engine build.
    target_include_directories(libgav1_static SYSTEM INTERFACE "${_gav1_dir}/src")
    set_target_properties(libgav1_static PROPERTIES FOLDER "third_party")
endfunction()
