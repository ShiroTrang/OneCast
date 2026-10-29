# RNNoise 0.2 (Xiph.Org, BSD-3-Clause) vendored from the official release
# tarball rnnoise-0.2.tar.gz, sha256
# 90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37.
#
# Built as a plain static C library. 48 kHz only, 480-sample frames, samples
# scaled to +-32768 (see include/rnnoise.h).
#
# Only the files the library needs are vendored: no training tools
# (dump_features.c, write_weights.c) and none of the x86 run-time CPU dispatch
# sources, so RNN_ENABLE_X86_RTCD stays undefined and the network always runs
# through nnet_default.c. vec.h still includes x86/x86_arch_macros.h, which is
# the one x86 file kept: it maps OPUS_X86_MAY_HAVE_SSE2 to __SSE2__ on MSVC,
# which selects the SSE2 kernels in vec_avx.h. MSVC does not define __SSE2__
# itself, and without it vec.h falls back to a generic path that includes an
# os_support.h the release tarball does not ship.

add_library(rnnoise STATIC
    src/denoise.c
    src/rnn.c
    src/pitch.c
    src/kiss_fft.c
    src/celt_lpc.c
    src/nnet.c
    src/nnet_default.c
    src/parse_lpcnet_weights.c
    src/rnnoise_data.c
    src/rnnoise_tables.c
)

set_target_properties(rnnoise PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(rnnoise
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(MSVC)
    target_compile_definitions(rnnoise PRIVATE
        OPUS_X86_MAY_HAVE_SSE2
        _CRT_SECURE_NO_WARNINGS
    )

    # Third-party code: no warnings. /O2 is skipped in Debug because it is
    # incompatible with the /RTC1 run-time checks CMake adds there.
    target_compile_options(rnnoise PRIVATE
        /W0
        $<$<NOT:$<CONFIG:Debug>>:/O2>
    )
else()
    target_compile_options(rnnoise PRIVATE -w -O2)
endif()
