# DSP node library (header nodes + factory) — pure C++, no PipeWire.
add_subdirectory(audio_core/dsp)   # → hermes_dsp

# Control / IO processes (no PipeWire). Each links hermes_common.
function(hermes_process name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE hermes_common)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

hermes_process(hermes_supervisor      supervisor/main.cpp supervisor/SessionFsm.cpp)
hermes_process(hermes_voice_trigger   voice_trigger/main.cpp)
hermes_process(hermes_video_proc      video_proc/main.cpp)
hermes_process(hermes_cloud_connector cloud_connector/main.cpp)
hermes_process(hermes_codec_hw        codec_hw/main.cpp)

# Audio process (hermes_abox): the MULTI-NODE DSP graph (src/aec/beamform/ses, each
# its own pw_filter) + AUDIO_CORE control, in ONE binary. Needs libpipewire-0.3, so
# it is built only when present (PipeWire links/schedules the nodes — init/WirePlumber).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PIPEWIRE QUIET libpipewire-0.3)
endif()
if(PIPEWIRE_FOUND)
    add_executable(hermes_abox
        audio_core/pipewire/Pw.cpp
        audio_core/pipewire/PwStage.cpp
        audio_core/pipewire/abox_main.cpp)
    target_include_directories(hermes_abox PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PIPEWIRE_INCLUDE_DIRS})
    target_link_libraries(hermes_abox PRIVATE hermes_dsp hermes_common ${PIPEWIRE_LIBRARIES})
    message(STATUS "Hermes: PipeWire found — building hermes_abox (multi-node audio core).")
else()
    message(STATUS "Hermes: PipeWire not found — hermes_abox (audio process) skipped.")
endif()
