cmake_minimum_required(VERSION 3.16)
project(Hermes LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # for clangd

option(HERMES_BUILD_TESTS "Build tests" ON)

add_subdirectory(common)   # libhermes_common — the IPC contract (CMsg/EventMap/MsgBus/Transport)
add_subdirectory(app)      # the 6 independent processes (SDS §14.2)

if(HERMES_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
