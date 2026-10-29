cmake_minimum_required(VERSION 3.22)
project(OneCast VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE lives outside the source tree when building from a git worktree
# (JUCE/ is gitignored), so its location is a cache variable and the build
# directory gets its own copy of the JUCE targets.
set(ONECAST_JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/JUCE" CACHE PATH "Path to the JUCE 8 source tree")
add_subdirectory(${ONECAST_JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)

# RNNoise 0.2, vendored (static library "rnnoise").
add_subdirectory(third_party/rnnoise)

# -----------------------------------------------------------------------------
# Source lists. Later tasks add files under these folders without touching this
# file, so the globs re-run on every build (CONFIGURE_DEPENDS) and tolerate a
# folder that holds only headers, or nothing yet.
#
# The Visual Studio generator notices a new file at the start of a build and
# regenerates the project, but that same build does not compile it. After adding
# a source file run `cmake -S . -B build` (the cache keeps the other options)
# before building, or build twice.
# -----------------------------------------------------------------------------
function(onecast_glob variable folder)
    file(GLOB_RECURSE found CONFIGURE_DEPENDS LIST_DIRECTORIES false
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/${folder}/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Source/${folder}/*.cpp")
    set(${variable} ${found} PARENT_SCOPE)
endfunction()

onecast_glob(ONECAST_DSP_SOURCES      Dsp)
onecast_glob(ONECAST_AUDIO_SOURCES    Audio)
onecast_glob(ONECAST_PLATFORM_SOURCES Platform)
onecast_glob(ONECAST_SESSION_SOURCES  Session)
onecast_glob(ONECAST_UI_SOURCES       UI)

file(GLOB ONECAST_TEST_SOURCES CONFIGURE_DEPENDS
    LIST_DIRECTORIES false
    "${CMAKE_CURRENT_SOURCE_DIR}/Tests/*.cpp")

# ASIO support requires the Steinberg ASIO SDK to be installed.
# Download from: https://www.steinberg.net/developers/
# Set ASIO_SDK_PATH to the SDK root, or leave empty to disable ASIO.
if(DEFINED ASIO_SDK_PATH AND IS_DIRECTORY "${ASIO_SDK_PATH}")
    message(STATUS "ASIO SDK found at: ${ASIO_SDK_PATH}")
    set(JUCE_ASIO_FLAG 1)
else()
    message(STATUS "ASIO SDK not found - ASIO support disabled (set -DASIO_SDK_PATH=... to enable)")
    set(JUCE_ASIO_FLAG 0)
endif()

# -----------------------------------------------------------------------------
# The app
# -----------------------------------------------------------------------------
juce_add_gui_app(OneCast
    PRODUCT_NAME "OneCast"
    COMPANY_NAME "OneCast"
    BUNDLE_ID "com.onecast.app"
)

target_sources(OneCast PRIVATE
    Source/Main.cpp
    Source/MainComponent.h
    Source/MainComponent.cpp
    Source/Diagnostics/StartupLog.h
    ${ONECAST_DSP_SOURCES}
    ${ONECAST_AUDIO_SOURCES}
    ${ONECAST_PLATFORM_SOURCES}
    ${ONECAST_SESSION_SOURCES}
    ${ONECAST_UI_SOURCES}
)

juce_generate_juce_header(OneCast)

if(JUCE_ASIO_FLAG)
    target_include_directories(OneCast PRIVATE "${ASIO_SDK_PATH}/common")
endif()

target_compile_definitions(OneCast PRIVATE
    JUCE_ASIO=${JUCE_ASIO_FLAG}
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=0
    JUCE_USE_WINRT_MIDI=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    # The app only reads and writes WAV, so the FLAC and Ogg Vorbis codecs
    # (each with another bundled library and licence) are left out of the
    # binary that ships.
    JUCE_USE_FLAC=0
    JUCE_USE_OGGVORBIS=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    NOMINMAX=1
    WIN32_LEAN_AND_MEAN=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:OneCast,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:OneCast,JUCE_VERSION>"
)

# The UI copy is Vietnamese; the sources are UTF-8 without a BOM.
target_compile_options(OneCast PRIVATE /utf-8)

target_link_libraries(OneCast PRIVATE
    rnnoise
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    mmdevapi
)

# -----------------------------------------------------------------------------
# Unit tests (no audio device needed). One console app runs every juce::UnitTest;
# pass a category to run only that one:
#   cmake --build build --config Release --target OneCastTests
#   build\OneCastTests_artefacts\Release\OneCastTests.exe [category]
# -----------------------------------------------------------------------------
juce_add_console_app(OneCastTests PRODUCT_NAME "OneCastTests")

target_sources(OneCastTests PRIVATE
    ${ONECAST_TEST_SOURCES}
    ${ONECAST_DSP_SOURCES}
    ${ONECAST_AUDIO_SOURCES}
    ${ONECAST_PLATFORM_SOURCES}
    ${ONECAST_SESSION_SOURCES}
)

juce_generate_juce_header(OneCastTests)

target_compile_definitions(OneCastTests PRIVATE
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=0
    JUCE_USE_WINRT_MIDI=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_FLAC=0
    JUCE_USE_OGGVORBIS=0
    NOMINMAX=1
    WIN32_LEAN_AND_MEAN=1
    # Absolute, so the fixtures are found from any build directory, including
    # one belonging to a git worktree.
    ONECAST_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Tests/data"
)

target_compile_options(OneCastTests PRIVATE /utf-8)

# tools/ProbeAnalyzer.h is shared with the OneCastProbe tool below.
target_include_directories(OneCastTests PRIVATE tools)

target_link_libraries(OneCastTests PRIVATE
    rnnoise
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_core
    juce::juce_dsp
    juce::juce_events
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    mmdevapi
)

# -----------------------------------------------------------------------------
# OneCastProbe: records "CABLE Output" and reports dropouts, phase jumps and
# level dips in a test sine (used by the soak test). The glitch detector itself
# is tools/ProbeAnalyzer.h, which OneCastTests covers without any device.
#   build\OneCastProbe_artefacts\Release\OneCastProbe.exe --self-test
# -----------------------------------------------------------------------------
juce_add_console_app(OneCastProbe PRODUCT_NAME "OneCastProbe")

target_sources(OneCastProbe PRIVATE
    tools/Probe.cpp
    tools/ProbeAnalyzer.h
)

juce_generate_juce_header(OneCastProbe)

target_compile_definitions(OneCastProbe PRIVATE
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=0
    JUCE_USE_WINRT_MIDI=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_FLAC=0
    JUCE_USE_OGGVORBIS=0
    NOMINMAX=1
    WIN32_LEAN_AND_MEAN=1
)

target_compile_options(OneCastProbe PRIVATE /utf-8)

target_link_libraries(OneCastProbe PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_core
    juce::juce_events
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    mmdevapi
)

# -----------------------------------------------------------------------------
# Offline renderer: the voice chain over a WAV file, with no audio device.
#   cmake --build build --config Release --target OneCastRender
#   build\OneCastRender_artefacts\Release\OneCastRender.exe in.wav out.wav
# -----------------------------------------------------------------------------
juce_add_console_app(OneCastRender PRODUCT_NAME "OneCastRender")

target_sources(OneCastRender PRIVATE
    tools/Render.cpp
    Source/Audio/SafetyLimiter.h
    ${ONECAST_DSP_SOURCES}
    ${ONECAST_SESSION_SOURCES}
)

juce_generate_juce_header(OneCastRender)

target_compile_definitions(OneCastRender PRIVATE
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=0
    JUCE_USE_WINRT_MIDI=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_FLAC=0
    JUCE_USE_OGGVORBIS=0
    NOMINMAX=1
    WIN32_LEAN_AND_MEAN=1
)

target_compile_options(OneCastRender PRIVATE /utf-8)

target_link_libraries(OneCastRender PRIVATE
    rnnoise
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_core
    juce::juce_dsp
    juce::juce_events
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
