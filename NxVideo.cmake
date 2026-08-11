# libvpx (VP9 decode) and libwebm (WebM demux), the two runtime codecs the
# video module needs. Neither is a submodule: they are only needed when
# NX_MODULE_VIDEO is on, and a submodule lands in every recursive clone whether
# or not the module is built. Fetched on demand, the way Spine is.
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

function(nx_add_video_codecs)
    include(FetchContent)

    set(_config "${CMAKE_CURRENT_LIST_DIR}/third_party/libvpx_config")

    # libvpx
    if (NX_LIBVPX_SOURCE_DIR)
        set(_vpx_dir "${NX_LIBVPX_SOURCE_DIR}")
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
    if (NX_LIBWEBM_SOURCE_DIR)
        set(_webm_dir "${NX_LIBWEBM_SOURCE_DIR}")
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
