#!/usr/bin/env bash
# run_gui.sh — launch the Hermes audio test console (web GUI) on top of the live engine.
#
# Brings up: PipeWire (if not already running) → hermes_abox (the engine) → hermes_gui_interface
# (the web bridge), then prints the URL. Open it and drive the basic use cases: pick a sample,
# Play (pw-play → hermes.abox), change engine mode, set volume, fire a barge-in, start a session.
#
#   ./scripts/run_gui.sh            # uses build-native/ binaries (run inside the arm64 container)
#   BIN_DIR=build-rk3588/app ...    # or point at another build
#
# On macOS, run this INSIDE the native container (it needs PipeWire + Linux mq):
#   IMAGE=ubuntu:24.04 ./scripts/build.sh native   # build first
#   ... then exec this script in a container that has pipewire + the build (see VALIDATION.md).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
BIN_DIR="${BIN_DIR:-build-native/app}"
PORT="${HERMES_GUI_PORT:-8080}"
export HERMES_SAMPLES_DIR="${HERMES_SAMPLES_DIR:-$ROOT/samples}"
export HERMES_GUI_PORT="$PORT"

SEARCH=("$BIN_DIR" build-native/app build-rk3588/app build/app)
ABOX="$(find "${SEARCH[@]}" -maxdepth 2 -name hermes_abox -type f 2>/dev/null | head -1 || true)"
GUI="$(find "${SEARCH[@]}" -maxdepth 2 -name hermes_gui_interface -type f 2>/dev/null | head -1 || true)"
[ -x "$GUI" ]  || { echo "error: hermes_gui_interface not found — build first (scripts/build.sh native)"; exit 1; }

# PipeWire (needed for pw-play and the engine). Start one if the socket isn't there.
if command -v pipewire >/dev/null 2>&1 && ! pgrep -x pipewire >/dev/null 2>&1; then
  export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/xdg}"; mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
  echo ">> starting pipewire"; pipewire >/tmp/pw.log 2>&1 & sleep 2
  command -v wireplumber >/dev/null 2>&1 && { wireplumber >/tmp/wp.log 2>&1 & sleep 1; }
fi

pids=()
if [ -x "$ABOX" ]; then echo ">> starting engine: $ABOX"; "$ABOX" & pids+=($!); sleep 1; else
  echo "!! hermes_abox not found — GUI will still run; control CMsgs will report 'no peer'."; fi
echo ">> starting GUI bridge: $GUI"; "$GUI" & pids+=($!)

trap 'echo; echo ">> stopping"; kill "${pids[@]}" 2>/dev/null || true' EXIT INT TERM
echo ">> Open  http://localhost:${PORT}   (Ctrl-C to stop)"
wait
