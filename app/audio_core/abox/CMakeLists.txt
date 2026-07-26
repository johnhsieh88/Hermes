# ── WebRTC audio processing (AEC3) — optional, preferred over the bypass stub ──
# Try the modern standalone package first, fall back to the legacy 0.x name.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(WEBRTC_AP QUIET IMPORTED_TARGET webrtc-audio-processing-1)
    if(NOT WEBRTC_AP_FOUND)
        pkg_check_modules(WEBRTC_AP QUIET IMPORTED_TARGET webrtc-audio-processing)
    endif()
endif()
if(WEBRTC_AP_FOUND)
    message(STATUS "Hermes ABOX: libwebrtc-audio-processing found — AEC3 enabled.")
else()
    message(STATUS "Hermes ABOX: libwebrtc-audio-processing not found — AEC bypass stub.")
endif()

# hermes_abox_c — the C data plane (SDD §2): vtable nodes, mask-gated graph tick,
# reference-manager delay ring, self-claim worker pool, lock-free param store. C11
# for all sources except aec_webrtc.cpp (C++17) which is included only when the
# WebRTC library is present. Both aec_node.c and aec_webrtc.cpp are always listed;
# each guards its symbols with #ifdef / #ifndef HERMES_HAVE_WEBRTC_AP so exactly
# one provides abox_aec_create() / abox_aec_set_ref() at link time.
add_library(hermes_abox_c STATIC
    abox_routing.c
    abox_graph.c
    reference_manager.c
    worker_pool.c
    abox_vdma.c
    buffer_pipeline.c
    abox_nodes.c              # factory: type name → constructor
    nodes/node_common.c       # shared allocator + default vtable slots
    nodes/src_node.c          # one node per file (SDS §4)
    nodes/aec_node.c              # bypass stub (empty when HERMES_HAVE_WEBRTC_AP)
    nodes/aec_webrtc.cpp          # AEC3 impl  (empty when NOT HERMES_HAVE_WEBRTC_AP)
    nodes/beamform_node.c         # 2→2 channel-preserving DAS (ARCH §13.2)
    nodes/dmx_node.c              # structural 2→1 downmix
    nodes/capgate_node.c)         # structural capture gate (mode gain × START/STOP_CAPTURE)
target_include_directories(hermes_abox_c PUBLIC ${CMAKE_SOURCE_DIR}/app
                                                ${CMAKE_SOURCE_DIR}/common/include)  # hermes/common/Log.h
target_compile_features(hermes_abox_c PUBLIC c_std_11)

if(WEBRTC_AP_FOUND)
    target_compile_definitions(hermes_abox_c PUBLIC  HERMES_HAVE_WEBRTC_AP)
    target_link_libraries     (hermes_abox_c PUBLIC  PkgConfig::WEBRTC_AP)
    # C++17 is needed for the aec_webrtc.cpp translation unit; propagate so that
    # consumers (hermes_abox executable) inherit the requirement automatically.
    target_compile_features   (hermes_abox_c PUBLIC  cxx_std_17)
endif()

# The worker pool runs one pinned pthread per big core.
find_package(Threads REQUIRED)
target_link_libraries(hermes_abox_c PUBLIC Threads::Threads)
if(UNIX AND NOT APPLE)
    target_link_libraries(hermes_abox_c PUBLIC m)   # sinf/sqrt for harness consumers
endif()

# Offline harness (SDS §5): runs the chain over synthetic frames, no PipeWire.
add_executable(hermes_offline_c harness_offline.c)
target_link_libraries(hermes_offline_c PRIVATE hermes_abox_c)
if(UNIX AND NOT APPLE)
    target_link_libraries(hermes_offline_c PRIVATE m)
endif()

# C self-test (assert-based; no GoogleTest) — registered with ctest if testing is on.
if(HERMES_BUILD_TESTS)
    add_executable(abox_selftest abox_selftest.c)
    target_link_libraries(abox_selftest PRIVATE hermes_abox_c)
    if(UNIX AND NOT APPLE)
        target_link_libraries(abox_selftest PRIVATE m)
    endif()
    add_test(NAME abox_selftest COMMAND abox_selftest)
endif()
