#!/usr/bin/env bash
# _loopback_run.sh — inner half of the loopback, run under dbus-run-session by loopback.sh.
# Starts PipeWire + WirePlumber + hermes_abox, wires the graph through the engine, streams a
# sample, captures the engine output, and reports whether real signal made it through.
set -e
export XDG_RUNTIME_DIR=/tmp/xdg
SAMPLE="${SAMPLE:-/src/samples/sweep_100_8000.wav}"
OUT=/src/out/loopback_out.wav
BIN="$(find /src/build-native -name hermes_abox -type f | head -1)"
rm -f "$OUT"

pipewire    >/tmp/pw.log 2>&1 & sleep 2
wireplumber >/tmp/wp.log 2>&1 & sleep 2
"$BIN"      >/tmp/abox.log 2>&1 & ABOX=$!
sleep 1.5

echo ">> wiring: engine_in.monitor → hermes.abox(in_0,in_1) → out_0 → engine_out"
pw-link engine_in:monitor_FL hermes.abox:in_0
pw-link engine_in:monitor_FR hermes.abox:in_1
pw-link hermes.abox:out_0    engine_out:playback_FL
echo "=== active links through the engine ==="
pw-link -l 2>/dev/null | grep -i hermes -A1 || true

echo ">> capture engine_out; play $(basename "$SAMPLE") into engine_in"
timeout 8 pw-record --target engine_out "$OUT" >/tmp/rec.log 2>&1 & REC=$!
sleep 0.6
timeout 8 pw-play --target engine_in "$SAMPLE" >/tmp/play.log 2>&1 || true
sleep 0.6
kill "$REC" 2>/dev/null || true; wait "$REC" 2>/dev/null || true
kill "$ABOX" 2>/dev/null || true

echo "=== captured file ==="
ls -l "$OUT" 2>/dev/null || { echo "no capture file"; exit 1; }
python3 -c '
import sys, wave, struct
w = wave.open(sys.argv[1]); n = w.getnframes(); ch = w.getnchannels()
d = w.readframes(n); s = struct.unpack("<%dh" % (len(d)//2), d) if d else []
pk = max((abs(x) for x in s), default=0)
ok = pk > 200
print(">> RESULT: %d frames, %dch, peak=%d (%.1f%% FS) -> %s"
      % (n, ch, pk, pk/32767*100, "SIGNAL PRESENT — abox+PipeWire loopback OK" if ok else "SILENT"))
sys.exit(0 if ok else 2)
' "$OUT"
echo ">> On macOS:  afplay out/loopback_out.wav   # hear the engine-processed audio"
