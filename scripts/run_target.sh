#!/usr/bin/env sh
# run_target.sh — full-path playback validation for hermes_abox ON the RK3588 target:
#   ALSA capture (mic) ─► hermes.abox engine (src→aec→beamform→ses) ─► ALSA playback (speaker)
# Requires PipeWire + WirePlumber running on the device. Run on the TARGET, not the host.
# The DSP nodes are passthrough stubs for now, so this validates the AUDIO PATH end-to-end
# (you should hear the mic, delayed ~1 block, out the speaker).
#
#   ./run_target.sh [path-to-hermes_abox] [seconds]
set -eu

BIN="${1:-./hermes_abox}"
SECS="${2:-10}"
NODE="hermes.abox"                       # engine ports: hermes.abox:in_0, in_1, out_0

[ -x "$BIN" ] || { echo "error: $BIN not found/executable"; exit 1; }
command -v pw-link >/dev/null 2>&1 || { echo "error: pipewire tools (pw-link) missing on target"; exit 1; }

echo ">> starting engine ($BIN)  [HERMES_SYNC=1 to force the inline path]"
"$BIN" &
ABOX_PID=$!
trap 'kill "$ABOX_PID" 2>/dev/null || true' EXIT
sleep 1.5

echo ">> engine ports:"; pw-link -o | grep -F "$NODE" || true; pw-link -i | grep -F "$NODE" || true

# Discover the default mic (capture_*) and speaker (playback_*) ports.
CAP_L=$(pw-link -o | grep -E 'capture_FL$'  | head -1 || true)
CAP_R=$(pw-link -o | grep -E 'capture_FR$'  | head -1 || true)
PB_L=$( pw-link -i | grep -E 'playback_FL$' | head -1 || true)
PB_R=$( pw-link -i | grep -E 'playback_FR$' | head -1 || true)

echo ">> linking mic → $NODE → speaker"
[ -n "$CAP_L" ] && pw-link "$CAP_L" "$NODE:in_0" 2>/dev/null || echo "   (no capture_FL — wire $NODE:in_0 manually)"
[ -n "$CAP_R" ] && pw-link "$CAP_R" "$NODE:in_1" 2>/dev/null || true
# mono engine output → both speaker channels
[ -n "$PB_L" ] && pw-link "$NODE:out_0" "$PB_L" 2>/dev/null || echo "   (no playback_FL — wire $NODE:out_0 manually)"
[ -n "$PB_R" ] && pw-link "$NODE:out_0" "$PB_R" 2>/dev/null || true

echo ">> path live for ${SECS}s — speak into the mic; you should hear it (~1 block delay)."
echo "   watch xruns/latency:  pw-top   (look for the '$NODE' row)"
echo "   (no mic? feed a file:  pw-play --target $NODE file.wav)"
sleep "$SECS"
echo ">> done."
