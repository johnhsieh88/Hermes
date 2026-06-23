# Cross-compile Hermes for arm64 (aarch64) Linux — the embedded target (SDS §9.5).
#
# Usage:
#   cmake -S . -B build-arm64 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.toolchain.cmake \
#         -DCMAKE_SYSROOT=/path/to/aarch64-sysroot
#   cmake --build build-arm64 -j
#
# The sysroot MUST contain libpipewire-0.3 + headers + its pkg-config (.pc) files.
# Get the sysroot from one of:
#   • Yocto SDK (recommended for the product): instead of this file, just
#       `source .../environment-setup-aarch64-poky-linux`
#     which sets CC/CXX, the sysroot, and PKG_CONFIG_* for you, then run cmake.
#   • A Debian/Ubuntu arm64 rootfs with `libpipewire-0.3-dev:arm64` installed
#     (multiarch or a debootstrap rootfs) → point CMAKE_SYSROOT at it.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Cross compilers (override the triple with -DCROSS_PREFIX=… if yours differs).
set(CROSS_PREFIX "aarch64-linux-gnu-" CACHE STRING "cross toolchain prefix")
set(CMAKE_C_COMPILER   "${CROSS_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${CROSS_PREFIX}g++")

# Resolve programs on the host, libs/headers/packages in the target sysroot.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config must find libpipewire-0.3 in the SYSROOT, not on the host.
if(CMAKE_SYSROOT)
  set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
  set(ENV{PKG_CONFIG_LIBDIR}
      "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
  unset(ENV{PKG_CONFIG_PATH})
endif()
