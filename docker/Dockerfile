# Cross-compile Hermes for the Rockchip RK3588 (aarch64) from any host — including
# macOS via Docker/colima. Ubuntu + arm64 multiarch + aarch64 cross toolchain +
# libpipewire-0.3-dev:arm64. Output is RK3588 (aarch64) binaries.
#
# Two ways to run it:
#
#   A) Docker Desktop "Run" button (no bind mount) — works because the repo is COPYed
#      into the image below. Cross-builds the baked-in snapshot; artifacts live inside
#      the container at /src/build-rk3588 (browse via Docker Desktop > Files, or
#      `docker cp <container>:/src/build-rk3588 ./out`).
#
#   B) CLI with a live bind mount (preferred for dev — artifacts land on the host):
#        docker build -t hermes-rk3588 -f docker/Dockerfile .
#        docker run --rm -v "$PWD":/src -w /src hermes-rk3588
#      The mounted volume shadows the COPYed snapshot, so it builds your live tree.
#
# This is a CROSS-COMPILE, not an emulated run: an aarch64 toolchain emits RK3588
# binaries; nothing aarch64 executes here, so no QEMU is involved in the build.
# Pinned to linux/amd64 so the multiarch cross-build is deterministic on any Mac
# (Intel or Apple Silicon). On Apple Silicon, Docker emulates amd64 for the build
# (slower but portable). See BUILD.md / VALIDATION.md.
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

# Bake the repo in so the image cross-builds with no bind mount (Docker Desktop "Run").
# .dockerignore keeps host build dirs / .git / the macOS CMakeCache out — a stale Darwin
# cache copied in here would poison the Linux configure. A bind mount (flow B) shadows this.
WORKDIR /src
COPY . /src

# Guard: if /src somehow has no CMakeLists.txt (e.g. an empty dir mounted over it),
# fail with an actionable message instead of cmake's cryptic "source directory ... ".
CMD if [ ! -f /src/CMakeLists.txt ]; then \
      echo "ERROR: /src has no CMakeLists.txt."; \
      echo "  Docker Desktop: just press Run (the repo is baked into the image)."; \
      echo "  CLI with live source: docker run --rm -v \"\$PWD\":/src -w /src hermes-rk3588"; \
      exit 2; \
    fi; \
    cmake -S /src -B /src/build-rk3588 \
        -DCMAKE_TOOLCHAIN_FILE=cmake/rk3588.toolchain.cmake \
        -DHERMES_BUILD_TESTS=OFF \
 && cmake --build /src/build-rk3588 -j \
 && echo ">> Done. aarch64 RK3588 binaries in /src/build-rk3588 (app/hermes_abox, app/hermes_supervisor, ...)."
