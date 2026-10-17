cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
if(NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Target architectures")
endif()

project(WEFT VERSION 1.0.0 LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(WEFT_ENGINE_ONLY "Build only the DSP engine and its tests (no JUCE)" OFF)
option(WEFT_SANITIZE "Build engine tests with address/undefined sanitizers" OFF)

# ── engine: plain C++, no JUCE ────────────────────────────────────────────────────────────────
file(GLOB WEFT_ENGINE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/modules/*.cpp)
add_library(weft_engine STATIC ${WEFT_ENGINE_SOURCES})
target_include_directories(weft_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(weft_engine PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-math-errno -fno-trapping-math
    $<$<CONFIG:Release>:-O3>)
set_target_properties(weft_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WEFT_SANITIZE)
    target_compile_options(weft_engine PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(weft_engine PUBLIC -fsanitize=address,undefined)
endif()

file(GLOB WEFT_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
add_executable(weft_tests ${WEFT_TEST_SOURCES})
target_link_libraries(weft_tests PRIVATE weft_engine)
target_include_directories(weft_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

if(WEFT_ENGINE_ONLY)
    return()
endif()

# ── plug-in ───────────────────────────────────────────────────────────────────────────────────
set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/JUCE EXCLUDE_FROM_ALL)

juce_add_plugin(WEFT
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "WEFT"
    COMPANY_WEBSITE "https://github.com/thematver/grid"
    BUNDLE_ID "com.thematver.weft"
    PLUGIN_MANUFACTURER_CODE Weft
    PLUGIN_CODE Wft1
    FORMATS AU VST3 Standalone
    PRODUCT_NAME "WEFT"
    DESCRIPTION "Modular synthesizer"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    AU_SANDBOX_SAFE TRUE
    VST3_CATEGORIES Instrument Synth
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "WEFT can process external audio through its INPUT module."
)

juce_add_binary_data(WeftAssets SOURCES
    assets/fonts/Geist-Regular.ttf
    assets/fonts/Geist-Medium.ttf
    assets/fonts/Geist-SemiBold.ttf
    assets/fonts/Geist-Bold.ttf
    assets/fonts/Geist-ExtraBold.ttf
    assets/fonts/GeistMono-Regular.ttf
    assets/fonts/GeistMono-Medium.ttf
    assets/fonts/GeistMono-SemiBold.ttf)

file(GLOB WEFT_PLUGIN_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.cpp)
target_sources(WEFT PRIVATE ${WEFT_PLUGIN_SOURCES})
target_include_directories(WEFT PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(WEFT PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0 JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_MODAL_LOOPS_PERMITTED=0
    JUCE_SILENCE_XCODE_15_LINKER_WARNING=1)
target_link_libraries(WEFT
    PRIVATE weft_engine WeftAssets
            juce::juce_audio_utils juce::juce_audio_formats
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

# Offscreen renderer used for pixel comparison against the design (tools/snapshot).
juce_add_console_app(weft_snapshot PRODUCT_NAME "weft_snapshot")
target_sources(weft_snapshot PRIVATE tools/snapshot/Snapshot.cpp ${WEFT_PLUGIN_SOURCES})
target_include_directories(weft_snapshot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(weft_snapshot PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1 WEFT_SNAPSHOT=1
    JucePlugin_Name="WEFT" JucePlugin_IsSynth=1 JucePlugin_WantsMidiInput=1 JucePlugin_ProducesMidiOutput=0
    JucePlugin_IsMidiEffect=0 JucePlugin_VersionString="${PROJECT_VERSION}")
target_link_libraries(weft_snapshot PRIVATE weft_engine WeftAssets
    juce::juce_audio_utils juce::juce_audio_formats juce::juce_recommended_config_flags)
