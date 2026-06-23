# hermes_common — the IPC contract linked by every process (SDS §14).
add_library(hermes_common STATIC
    src/MsgBus.cpp
    src/Transport.cpp)

target_include_directories(hermes_common PUBLIC include)
target_compile_features(hermes_common PUBLIC cxx_std_17)

if(UNIX AND NOT APPLE)
    target_link_libraries(hermes_common PUBLIC rt pthread)   # POSIX mq + shm + threads
else()
    find_package(Threads REQUIRED)
    target_link_libraries(hermes_common PUBLIC Threads::Threads)
endif()
