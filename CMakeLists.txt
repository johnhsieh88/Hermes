cmake_minimum_required(VERSION 3.16)
project(Hermes LANGUAGES C CXX)   # C for the RT data path (abox node/graph), C++ for control

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)          # C11: _Atomic / stdatomic for the lock-free data plane
set(CMAKE_C_STANDARD_REQUIRED ON)

# An implicit function declaration in C silently assumes int return → a 64-bit pointer
# gets truncated to 32 bits (a real segfault we hit). Make it a hard error, never a warning.
add_compile_options($<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # for clangd

option(HERMES_BUILD_TESTS "Build tests" ON)

# enable_testing() must precede any add_test() — including the C abox_selftest under app/.
if(HERMES_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(common)   # libhermes_common — the IPC contract (CMsg/EventMap/MsgBus/Transport)
add_subdirectory(app)      # the 6 independent processes (SDS §14.2) + C data plane (abox)

if(HERMES_BUILD_TESTS)
    add_subdirectory(test)
endif()
