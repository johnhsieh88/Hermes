# Tests use GoogleTest if available; otherwise the test tree is skipped so the
# core build (common + app) always succeeds offline.
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    message(STATUS "Hermes: GTest not found — skipping tests (install gtest to enable).")
    return()
endif()

function(hermes_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE hermes_common GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/app)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

hermes_test(test_eventmap unit/common/test_eventmap.cpp)

# Primary KPIs (SDS §17.2) — the two key paths.
hermes_test(barge_in_e2e  integration/barge_in_e2e.cpp)
hermes_test(kwd_wake_e2e  integration/kwd_wake_e2e.cpp)
