# Cross-compile Hermes for the Rockchip RK3588 (aarch64) from any host — including
# macOS via Docker/colima. Ubuntu + arm64 multiarch + aarch64 cross toolchain +
# libpipewire-0.3-dev:arm64. Output is RK3588 (aarch64) binaries.
#
#   docker build -t hermes-rk3588 -f docker/Dockerfile .
#   docker run --rm -v "$PWD":/src -w /src hermes-rk3588        # cross-builds (default CMD)
#
# Pinned to linux/amd64 so the multiarch cross-build is deterministic on any Mac
# (Intel or Apple Silicon). On Apple Silicon you may instead build natively in an
# arm64 container — see BUILD.md.
FROM --platform=linux/amd64 ubuntu:24.04

RUN dpkg --add-architecture arm64 \
 && sed -i 's/^Types: deb/Types: deb\nArchitectures: amd64/' /etc/apt/sources.list.d/ubuntu.sources \
 && printf 'Types: deb\nURIs: http://ports.ubuntu.com/ubuntu-ports\nSuites: noble noble-updates noble-backports\nComponents: main universe\nArchitectures: arm64\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n' \
      > /etc/apt/sources.list.d/ports-arm64.sources \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      cmake make pkg-config git \
      g++-aarch64-linux-gnu \
      libpipewire-0.3-dev:arm64 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
CMD cmake -S . -B build-rk3588 \
        -DCMAKE_TOOLCHAIN_FILE=cmake/rk3588.toolchain.cmake \
        -DHERMES_BUILD_TESTS=OFF \
 && cmake --build build-rk3588 -j
