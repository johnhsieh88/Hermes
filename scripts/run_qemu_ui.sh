#!/usr/bin/env bash
# run_qemu_ui.sh — build Hermes for arm64 in Docker (QEMU emulation) and run the full
# voice pipeline + web UI so you can test the whole path from a browser.
#
# Usage:
#   ./scripts/run_qemu_ui.sh               # build + run
#   SKIP_BUILD=1 ./scripts/run_qemu_ui.sh  # skip rebuild if already built
#   PORT=9090 ./scripts/run_qemu_ui.sh     # override GUI port (default 8080)
#
# What runs inside the container:
#   pipewire (null sink) → hermes_abox (DSP: beamform+AEC+capgate)
#                        → hermes_supervisor (session FSM)
#                        → hermes_llm_connector (resident STT + Groq + Piper TTS)
#                        → hermes_gui_interface (web UI at PORT)
#
# Open http://localhost:PORT in your browser.
# Use "Speak to Aria" card to test the full pipeline.
# GROQ_API_KEY must be set in the environment (or ~/.ensoul-secrets) for LLM replies.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-8080}"
IMAGE="${IMAGE:-ubuntu:24.04}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

# Load Groq key from ~/.ensoul-secrets if not already in env
if [ -z "${GROQ_API_KEY:-}" ] && [ -f "$HOME/.ensoul-secrets" ]; then
    export GROQ_API_KEY="$(grep ENSOUL_GROQ_KEY "$HOME/.ensoul-secrets" | cut -d= -f2- | tr -d '"' | tr -d "'" || true)"
fi
GROQ_API_KEY="${GROQ_API_KEY:-}"

command -v docker >/dev/null || { echo "error: docker not found"; exit 1; }

if [ "${SKIP_BUILD:-0}" != "1" ]; then
    echo ">> Building Hermes for arm64 in Docker (QEMU — this takes a few minutes)…"
    docker run --rm --platform=linux/arm64 \
        -v "$ROOT":/src -w /src \
        -e JOBS="$JOBS" \
        "$IMAGE" bash -s <<'BUILD'
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends \
    cmake make g++ pkg-config \
    libpipewire-0.3-dev pipewire wireplumber \
    libwebrtc-audio-processing-dev \
    libcurl4-openssl-dev \
    sherpa-onnx 2>/dev/null || true
# sherpa-onnx likely not in apt; build will fall back to stub — that is fine for pipeline test

BUILD_DIR=/src/build-qemu
cmake -S /src -B $BUILD_DIR \
    -DCMAKE_BUILD_TYPE=Release \
    -DHERMES_BUILD_TESTS=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF 2>&1 | tail -20
cmake --build $BUILD_DIR -j${JOBS} 2>&1
echo ">> Build done. Binaries in build-qemu/app/"
BUILD
fi

echo ">> Starting pipeline in arm64 container (port $PORT forwarded)…"
echo ">> Open http://localhost:${PORT} in your browser."
echo ">> Ctrl-C to stop."
echo ""

docker run --rm --platform=linux/arm64 \
    -p "${PORT}:${PORT}" \
    -v "$ROOT":/src -w /src \
    -e HERMES_GUI_PORT="$PORT" \
    -e HERMES_SAMPLES_DIR=/src/samples \
    -e GROQ_API_KEY="$GROQ_API_KEY" \
    -e XDG_RUNTIME_DIR=/tmp/xdg \
    "$IMAGE" bash -s <<RUNSCRIPT
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq -o Acquire::Check-Valid-Until=false 2>/dev/null
apt-get install -y --no-install-recommends pipewire wireplumber 2>/dev/null || true

mkdir -p /tmp/xdg && chmod 700 /tmp/xdg
export XDG_RUNTIME_DIR=/tmp/xdg

BIN=/src/build-qemu/app
[ -x "\$BIN/hermes_gui_interface" ] || { echo "error: build-qemu/app/hermes_gui_interface not found — run without SKIP_BUILD=1"; exit 1; }

pids=()
cleanup() { for p in "\${pids[@]:-}"; do kill "\$p" 2>/dev/null || true; done; }
trap cleanup EXIT INT TERM

echo ">> pipewire (null sink)"
pipewire --config-name=pipewire-pulse.conf >/tmp/pw.log 2>&1 & pids+=(\$!); sleep 2

if command -v wireplumber >/dev/null 2>&1; then
    wireplumber >/tmp/wp.log 2>&1 & pids+=(\$!); sleep 1
fi

if [ -x "\$BIN/hermes_abox" ]; then
    echo ">> hermes_abox (DSP engine)"
    "\$BIN/hermes_abox" >/tmp/abox.log 2>&1 & pids+=(\$!); sleep 1
fi

if [ -x "\$BIN/hermes_supervisor" ]; then
    echo ">> hermes_supervisor (session FSM)"
    "\$BIN/hermes_supervisor" >/tmp/sup.log 2>&1 & pids+=(\$!); sleep 0.5
fi

if [ -x "\$BIN/hermes_llm_connector" ]; then
    echo ">> hermes_llm_connector (STT + Groq + TTS)"
    HERMES_PW_CAP_TARGET=hermes.abox "\$BIN/hermes_llm_connector" >/tmp/cc.log 2>&1 & pids+=(\$!); sleep 1
fi

if [ -x "\$BIN/hermes_voice_trigger" ]; then
    echo ">> hermes_voice_trigger (KWD — optional)"
    "\$BIN/hermes_voice_trigger" >/tmp/vt.log 2>&1 & pids+=(\$!)
fi

echo ">> hermes_gui_interface on http://localhost:${PORT}"
"\$BIN/hermes_gui_interface"
RUNSCRIPT
