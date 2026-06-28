#!/usr/bin/env bash
# run_loopback.sh — REAL hermes_abox + PipeWire loopback on your Mac (no audio hardware).
# Streams a sample through the live engine inside an arm64 container and writes the engine
# OUTPUT to out/loopback_out.wav. Then on macOS:  afplay out/loopback_out.wav  to HEAR it.
#
#   ./scripts/run_loopback.sh                       # default sample (sweep)
#   SAMPLE=/src/samples/beeps_x3.wav ./scripts/run_loopback.sh
#
# This is the closest thing to the on-device ALSA→DSP→speaker loopback that runs on a Mac:
# the audio genuinely flows through hermes.abox via PipeWire; only the final speaker is
# replaced by a capture-to-file (a container has no audio device — see VALIDATION.md).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
IMAGE="${IMAGE:-ubuntu:24.04}"
command -v docker >/dev/null || { echo "error: docker not found / not running"; exit 1; }

if [ ! -x build-native/app/hermes_abox ]; then
  echo ">> native build missing — building first"
  ./scripts/build.sh native
fi

echo ">> running abox+PipeWire loopback in $IMAGE (~30s incl. apt)"
docker run --rm --platform=linux/arm64 -e SAMPLE="${SAMPLE:-}" \
  -v "$ROOT":/src -w /src "$IMAGE" bash /src/scripts/loopback.sh

echo
echo ">> done. Hear the engine-processed audio:   afplay out/loopback_out.wav"
