add_subdirectory(audio_core/abox)

function(hermes_process name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE hermes_common)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

hermes_process(hermes_supervisor      supervisor/main.cpp supervisor/SessionFsm.cpp)
hermes_process(hermes_video_proc      video_proc/main.cpp)
hermes_process(hermes_codec_hw        codec_hw/main.cpp)
hermes_process(hermes_story_agent     story_agent/main.cpp)

# Dev/test web bridge (ModuleId 7)
hermes_process(hermes_gui_interface   gui_interface/gui_main.cpp)
target_include_directories(hermes_gui_interface PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/gui_interface)

# ── PipeWire detection (shared by abox, voice_trigger, cloud_connector) ──────
# Skip pkg_check_modules when the toolchain (cross-compile) pre-seeds PIPEWIRE_FOUND
# so pkg-config doesn't overwrite PIPEWIRE_LIBRARIES with a host -l flag.
if(NOT PIPEWIRE_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PIPEWIRE QUIET libpipewire-0.3)
    endif()
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

# Skip host search when the toolchain pre-seeds SHERPA_C_LIB/SHERPA_C_INC
if(NOT SHERPA_C_LIB)
    find_library(SHERPA_C_LIB NAMES sherpa-onnx-c-api
        PATHS /usr/lib /usr/local/lib
        PATH_SUFFIXES "" "sherpa-onnx")
endif()
if(NOT SHERPA_C_INC)
    find_path(SHERPA_C_INC "sherpa-onnx/c-api/c-api.h"
        PATHS /usr/include /usr/local/include)
endif()
if(SHERPA_C_LIB AND SHERPA_C_INC)
    target_include_directories(hermes_voice_trigger PRIVATE ${SHERPA_C_INC})
    target_link_libraries(hermes_voice_trigger PRIVATE ${SHERPA_C_LIB})
    target_compile_definitions(hermes_voice_trigger PRIVATE HERMES_HAVE_SHERPA_ONNX)
    message(STATUS "Hermes VTS: sherpa-onnx C API found — KWD enabled.")
else()
    message(STATUS "Hermes VTS: sherpa-onnx C API not found — KWD disabled (PTT only).")
endif()

# ── hermes_llm_connector: PipeWire streams + libcurl for Groq ────────────────
add_executable(hermes_llm_connector llm_connector/main.cpp)
target_link_libraries(hermes_llm_connector PRIVATE hermes_common)
target_include_directories(hermes_llm_connector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(PIPEWIRE_FOUND)
    target_sources(hermes_llm_connector PRIVATE audio_core/pipewire/Pw.cpp)
    target_include_directories(hermes_llm_connector PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
    target_link_libraries(hermes_llm_connector PRIVATE ${PIPEWIRE_LIBRARIES})
    target_compile_definitions(hermes_llm_connector PRIVATE HERMES_HAVE_PIPEWIRE)
endif()

if(NOT CURL_FOUND)
    find_package(CURL QUIET)
endif()
if(CURL_FOUND)
    # Use CURL_LIBRARIES directly (not CURL::libcurl) to avoid the CMake imported
    # target pulling in host x86_64 include paths during cross-compilation.
    target_include_directories(hermes_llm_connector PRIVATE ${CURL_INCLUDE_DIRS})
    target_link_libraries(hermes_llm_connector PRIVATE ${CURL_LIBRARIES})
    target_compile_definitions(hermes_llm_connector PRIVATE HERMES_HAVE_CURL)
    message(STATUS "Hermes LLM: libcurl found — Groq LLM enabled.")
else()
    message(STATUS "Hermes LLM: libcurl not found — LLM will echo input.")
endif()

# Resident streaming STT: reuse the sherpa-onnx C API found for VTS above.
if(SHERPA_C_LIB AND SHERPA_C_INC)
    target_include_directories(hermes_llm_connector PRIVATE ${SHERPA_C_INC})
    target_link_libraries(hermes_llm_connector PRIVATE ${SHERPA_C_LIB})
    target_compile_definitions(hermes_llm_connector PRIVATE HERMES_HAVE_SHERPA_ONNX)
    message(STATUS "Hermes LLM: sherpa-onnx C API found — resident streaming STT enabled.")
else()
    message(STATUS "Hermes LLM: sherpa-onnx not found — STT falls back to subprocess.")
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
