#!/usr/bin/env bash
# Build (and optionally smoke-run) Hermes via Docker.
#
# Hermes targets the Rockchip RK3588 (aarch64) and the audio core needs PipeWire
# (Linux-only). On macOS you build inside Docker. See BUILD.md for details.
#
#   ./scripts/build.sh                 # cross-compile for RK3588 (aarch64)  -> build-rk3588/
#   ./scripts/build.sh native          # native build in an arm64 container  -> build/
#   ./scripts/build.sh run             # native build + headless smoke-run of hermes_abox
#   ./scripts/build.sh test            # native build + run the unit tests (ctest)
#
# Env overrides: IMAGE (base image), JOBS (parallel build jobs), RUN_SECS (smoke duration).
set -euo pipefail

MODE="${1:-cross}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${IMAGE:-ubuntu:24.04}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
RUN_SECS="${RUN_SECS:-5}"

cd "$ROOT"
command -v docker >/dev/null || { echo "error: docker not found / not running"; exit 1; }

# Cross-compile for RK3588 (aarch64). Produces device binaries; not runnable on this host.
if [ "$MODE" = "cross" ]; then
  echo ">> Cross-compiling for RK3588 (aarch64) via docker/Dockerfile"
  docker build -t hermes-rk3588 -f docker/Dockerfile .
  docker run --rm -v "$ROOT":/src -w /src hermes-rk3588
  echo ">> Done. aarch64 binaries in build-rk3588/ (flash to device; won't run on this host)."
  exit 0
fi

# Native arm64 build/run/test in a throwaway container (works on Apple Silicon).
case "$MODE" in
  native) DEPS="cmake make g++ pkg-config libpipewire-0.3-dev"; TESTS=OFF ;;
  test)   DEPS="cmake make g++ pkg-config libpipewire-0.3-dev libgtest-dev"; TESTS=ON  ;;
  run)    DEPS="cmake make g++ pkg-config libpipewire-0.3-dev pipewire wireplumber"; TESTS=OFF ;;
  *)      echo "usage: $0 {cross|native|run|test}"; exit 2 ;;
esac

echo ">> Native arm64 '$MODE' in $IMAGE"
docker run --rm --platform=linux/arm64 -v "$ROOT":/src -w /src \
  -e MODE="$MODE" -e TESTS="$TESTS" -e DEPS="$DEPS" -e JOBS="$JOBS" -e RUN_SECS="$RUN_SECS" \
  "$IMAGE" bash -s <<'EOS'
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends $DEPS >/dev/null

# Dedicated container build dir — NEVER share ./build with a macOS host build, or the
# stale Darwin CMakeCache poisons the Linux configure (wrong compiler/paths, no rebuild).
BUILD=/src/build-native
cmake -S /src -B $BUILD -DHERMES_BUILD_TESTS=$TESTS
cmake --build $BUILD -j$JOBS

case "$MODE" in
  native)
    echo ">> Built. Binaries in build-native/app/"
    ;;
  test)
    ctest --test-dir $BUILD --output-on-failure
    ;;
  run)
    export XDG_RUNTIME_DIR=/tmp/xdg; mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
    pipewire >/tmp/pw.log 2>&1 & sleep 3
    BIN=$(find $BUILD -name hermes_abox | head -1)
    echo ">> Running $BIN for ${RUN_SECS}s"
    rc=0; timeout "$RUN_SECS" "$BIN" || rc=$?
    echo ">> hermes_abox exit: $rc (124=stayed connected=GOOD, 1=could not reach PipeWire)"
    [ "$rc" = 124 ] && exit 0 || exit "$rc"
    ;;
esac
EOS
