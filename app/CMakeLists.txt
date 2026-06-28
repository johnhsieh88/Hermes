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

hermes_process(hermes_supervisor      supervisor/main.cpp supervisor/SessionFsm.cpp)
hermes_process(hermes_voice_trigger   voice_trigger/main.cpp)
hermes_process(hermes_video_proc      video_proc/main.cpp)
hermes_process(hermes_cloud_connector cloud_connector/main.cpp)
hermes_process(hermes_codec_hw        codec_hw/main.cpp)

# Audio process (hermes_abox): ONE PipeWire filter whose on_process drives the C buffer-pool
# coordinator over the abox_node graph (Model B). The bridge (abox_main.cpp) is the only
# pw_* boundary. Built only when libpipewire-0.3 is present.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PIPEWIRE QUIET libpipewire-0.3)
endif()
if(PIPEWIRE_FOUND)
    add_executable(hermes_abox
        audio_core/pipewire/Pw.cpp
        audio_core/pipewire/abox_main.cpp)
    target_include_directories(hermes_abox PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PIPEWIRE_INCLUDE_DIRS})
    target_link_libraries(hermes_abox PRIVATE hermes_abox_c hermes_common ${PIPEWIRE_LIBRARIES})
    message(STATUS "Hermes: PipeWire found — building hermes_abox (C buffer-pool engine).")
else()
    message(STATUS "Hermes: PipeWire not found — hermes_abox (audio process) skipped.")
endif()
