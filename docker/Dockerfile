# Reproducible Linux build environment with PipeWire (x86_64). Lets you compile +
# verify the PipeWire targets from macOS once a container runtime is installed
# (e.g. `brew install colima docker && colima start`).
#
#   docker build -t hermes-build -f docker/Dockerfile .
#   docker run --rm -v "$PWD":/src -w /src hermes-build \
#       bash -c "cmake -S . -B build && cmake --build build -j && ctest --test-dir build"
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake g++ pkg-config make git \
        libpipewire-0.3-dev libgtest-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
