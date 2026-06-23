# The §1–§13 DSP island as a pure C++ library (no PipeWire dependency).
add_subdirectory(audio_core/dsp)   # → hermes_dsp

# Six independent process executables (SDS §14.2). Each links hermes_common.
function(hermes_process name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE hermes_common)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

hermes_process(hermes_supervisor      supervisor/main.cpp supervisor/SessionFsm.cpp)
hermes_process(hermes_audio_core      audio_core/main.cpp)
hermes_process(hermes_voice_trigger   voice_trigger/main.cpp)
hermes_process(hermes_video_proc      video_proc/main.cpp)
hermes_process(hermes_cloud_connector cloud_connector/main.cpp)
hermes_process(hermes_codec_hw        codec_hw/main.cpp)

# ABOX runs the DSP chain (SRC/AEC/Beamform/SES).
target_link_libraries(hermes_audio_core PRIVATE hermes_dsp)

# Optional PipeWire SPA node for audio_core (the data plane). When libpipewire-0.3
# is present, build the real node; otherwise audio_core builds the DSP chain alone
# so the tree compiles offline.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PIPEWIRE QUIET libpipewire-0.3)
endif()
if(PIPEWIRE_FOUND)
    # Single-node RT island (§14.4): the whole Chain in one SPA node.
    target_sources(hermes_audio_core PRIVATE audio_core/pipewire/PwNode.cpp)
    target_include_directories(hermes_audio_core PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
    target_link_libraries(hermes_audio_core PRIVATE ${PIPEWIRE_LIBRARIES})
    target_compile_definitions(hermes_audio_core PRIVATE HERMES_HAVE_PIPEWIRE=1)

    # Multi-node variant: one PipeWire-node executable per DSP stage (separate
    # sources). Links between them are established by the init process / WirePlumber.
    foreach(stage src aec beamform ses chanmix)
        add_executable(hermes_pw_${stage}
            audio_core/pipewire/Pw.cpp
            audio_core/pipewire/PwStage.cpp
            audio_core/pipewire/nodes/${stage}_node.cpp)
        target_include_directories(hermes_pw_${stage}
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PIPEWIRE_INCLUDE_DIRS})
        target_link_libraries(hermes_pw_${stage}
            PRIVATE hermes_dsp hermes_common ${PIPEWIRE_LIBRARIES})
    endforeach()

    # Single ABOX binary hosting the SAME multi-node graph in ONE process (one
    # PipeWire client/loop, N pw_filter nodes). Preferred deploy: one binary.
    add_executable(hermes_abox
        audio_core/pipewire/Pw.cpp
        audio_core/pipewire/PwStage.cpp
        audio_core/pipewire/abox_main.cpp)
    target_include_directories(hermes_abox PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PIPEWIRE_INCLUDE_DIRS})
    target_link_libraries(hermes_abox PRIVATE hermes_dsp hermes_common ${PIPEWIRE_LIBRARIES})

    message(STATUS "Hermes: PipeWire found — building single-node island, per-stage nodes, and hermes_abox.")
else()
    message(STATUS "Hermes: PipeWire not found — audio_core builds DSP chain without the SPA node.")
endif()

# voice_trigger / cloud_connector / video_proc are PipeWire clients too (add the
# same pkg-config guard when their capture/playback paths are implemented).
