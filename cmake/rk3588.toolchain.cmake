# Rockchip RK3588 — 4× Cortex-A76 (big) + 4× Cortex-A55 (LITTLE), aarch64 / ARMv8.2-A.
# Matches the SDS §2 big.LITTLE 8-core topology. Extends the generic aarch64 toolchain
# with A76/A55 tuning (NEON is implied on aarch64).
#
# Usage:
#   cmake -S . -B build-rk3588 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/rk3588.toolchain.cmake \
#         -DCMAKE_SYSROOT=/path/to/rk3588-arm64-sysroot
#   cmake --build build-rk3588 -j
#
# (Or use the Yocto meta-rockchip SDK instead: `source environment-setup-*` then cmake.)

include(${CMAKE_CURRENT_LIST_DIR}/aarch64-linux-gnu.toolchain.cmake)

# DynamIQ heterogeneous tuning: schedule for A76, stay compatible with A55.
# Requires GCC >= 10 / Clang >= 11. Fall back to -mcpu=cortex-a76 on older toolchains.
set(CMAKE_C_FLAGS_INIT   "-mcpu=cortex-a76.cortex-a55")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a76.cortex-a55")
