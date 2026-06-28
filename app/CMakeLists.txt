# C data plane (SDD §2 / §18): node processing + data path in C — vtable nodes, mask-gated
# graph tick, Core-Proportional Buffer Pool, reference manager, self-claim worker pool,
# param store. This is the live engine. → hermes_abox_c (+ hermes_offline_c, abox_selftest).
add_subdirectory(audio_core/abox)

# Control / IO processes (no PipeWire). Each links hermes_common.
function(hermes_process name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE hermes_common)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

hermes_process(hermes_supervisor  supervisor/main.cpp supervisor/SessionFsm.cpp)
hermes_process(hermes_video_proc  video_proc/main.cpp)
hermes_process(hermes_codec_hw    codec_hw/main.cpp)

# ── PipeWire detection (shared by abox, voice_trigger, cloud_connector) ──────
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PIPEWIRE QUIET libpipewire-0.3)
endif()

# ── hermes_voice_trigger: PipeWire mic tap + sherpa-onnx KWD ─────────────────
add_executable(hermes_voice_trigger voice_trigger/main.cpp)
target_link_libraries(hermes_voice_trigger PRIVATE hermes_common)
target_include_directories(hermes_voice_trigger PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(PIPEWIRE_FOUND)
    target_sources(hermes_voice_trigger PRIVATE audio_core/pipewire/Pw.cpp)
    target_include_directories(hermes_voice_trigger PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
    target_link_libraries(hermes_voice_trigger PRIVATE ${PIPEWIRE_LIBRARIES})
    target_compile_definitions(hermes_voice_trigger PRIVATE HERMES_HAVE_PIPEWIRE)
endif()

# sherpa-onnx C API for keyword spotting
find_library(SHERPA_C_LIB NAMES sherpa-onnx-c-api
    PATHS /usr/lib /usr/local/lib
    PATH_SUFFIXES "" "sherpa-onnx")
find_path(SHERPA_C_INC "sherpa-onnx/c-api/c-api.h"
    PATHS /usr/include /usr/local/include)
if(SHERPA_C_LIB AND SHERPA_C_INC)
    target_include_directories(hermes_voice_trigger PRIVATE ${SHERPA_C_INC})
    target_link_libraries(hermes_voice_trigger PRIVATE ${SHERPA_C_LIB})
    target_compile_definitions(hermes_voice_trigger PRIVATE HERMES_HAVE_SHERPA_ONNX)
    message(STATUS "Hermes VTS: sherpa-onnx C API found — KWD enabled.")
else()
    message(STATUS "Hermes VTS: sherpa-onnx C API not found — KWD disabled (PTT only).")
endif()

# ── hermes_cloud_connector: PipeWire streams + libcurl for Groq ──────────────
add_executable(hermes_cloud_connector cloud_connector/main.cpp)
target_link_libraries(hermes_cloud_connector PRIVATE hermes_common)
target_include_directories(hermes_cloud_connector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(PIPEWIRE_FOUND)
    target_sources(hermes_cloud_connector PRIVATE audio_core/pipewire/Pw.cpp)
    target_include_directories(hermes_cloud_connector PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
    target_link_libraries(hermes_cloud_connector PRIVATE ${PIPEWIRE_LIBRARIES})
    target_compile_definitions(hermes_cloud_connector PRIVATE HERMES_HAVE_PIPEWIRE)
endif()

find_package(CURL QUIET)
if(CURL_FOUND)
    target_link_libraries(hermes_cloud_connector PRIVATE CURL::libcurl)
    target_compile_definitions(hermes_cloud_connector PRIVATE HERMES_HAVE_CURL)
    message(STATUS "Hermes CC: libcurl found — Groq LLM enabled.")
else()
    message(STATUS "Hermes CC: libcurl not found — LLM will echo input.")
endif()

# ── hermes_abox: ONE PipeWire filter driving the C buffer-pool engine ─────────
if(PIPEWIRE_FOUND)
    add_executable(hermes_abox
        audio_core/pipewire/Pw.cpp
        audio_core/pipewire/abox_main.cpp)
    target_include_directories(hermes_abox PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PIPEWIRE_INCLUDE_DIRS})
    target_link_libraries(hermes_abox PRIVATE hermes_abox_c hermes_common ${PIPEWIRE_LIBRARIES})
    message(STATUS "Hermes: PipeWire found — building hermes_abox.")
else()
    message(STATUS "Hermes: PipeWire not found — hermes_abox + audio processes skipped.")
endif()
