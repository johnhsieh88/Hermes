# hermes_abox_c — the C data plane (SDD §2): vtable nodes, mask-gated graph tick,
# reference-manager delay ring, self-claim worker pool, lock-free param store. C11.
add_library(hermes_abox_c STATIC
    abox_routing.c
    abox_graph.c
    reference_manager.c
    worker_pool.c
    buffer_pipeline.c
    abox_nodes.c              # factory: type name → constructor
    nodes/node_common.c       # shared allocator + default vtable slots
    nodes/src_node.c          # one node per file (SDS §4)
    nodes/aec_node.c
    nodes/beamform_node.c
    nodes/ses_node.c)
target_include_directories(hermes_abox_c PUBLIC ${CMAKE_SOURCE_DIR}/app)
target_compile_features(hermes_abox_c PUBLIC c_std_11)

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
