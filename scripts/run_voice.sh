#!/usr/bin/env sh
# run_voice.sh — bring up the FULL capture→STT→cloud path (UC-12, ARCHITECTURE §16.8)
# on the target/QEMU: abox engine + mic links + supervisor + VTS + llm_connector, with
# the connector's capture stream linked to abox's clean out_0.
#
#   ./run_voice.sh [bin-dir]         # dir containing the hermes_* binaries (default .)
#
# Env knobs:
#   HERMES_TEST_UTTERANCE="hello"    # bypass STT (no model needed) — CI/smoke mode
#   GROQ_API_KEY=...                 # cloud LLM (else replies echo)
#   HERMES_SYNC=1                    # abox inline path (no worker pool)
#   HERMES_ABOX_TRACE=200            # 1 Hz per-node trace (DEV ONLY)
#
# What "working" looks like (the UC-12 validation checklist, grep the log):
#   [CC] resident STT ready (…)                        ← model loaded once, at boot
#   [SUP] WAKE_CONFIRMED received (state=IDLE)         ← say "hey aria …"
#   … START_CAPTURE → capgate open                     ← the gate opens (abox)
#   … preroll: backfill N ms queued (wake_pos=…)       ← ring history spliced
#   … vad: … ring N smp (… ms, …% of 2000 ms cap) …    ← buffering healthy (over=0)
#   … turn finalized: N ms audio decoded | ring hw=…   ← transcript ~instant at endpoint
#   [CC] transcript: '…'                               ← the string that goes to the cloud
#   [CC] reply: …                                      ← and what came back
#   … STOP_CAPTURE → capgate closed … state SPEAK → IDLE
set -eu

BIN="${1:-.}"
NODE="hermes.abox"

command -v pw-link >/dev/null 2>&1 || { echo "error: pipewire tools missing"; exit 1; }
for b in hermes_abox hermes_supervisor hermes_llm_connector; do
    [ -x "$BIN/$b" ] || { echo "error: $BIN/$b not found"; exit 1; }
done

PIDS=""
cleanup() { for p in $PIDS; do kill "$p" 2>/dev/null || true; done; }
trap cleanup EXIT INT TERM

echo ">> abox engine (idle mode; the FSM drives CONVERSATION per turn)"
"$BIN/hermes_abox" & PIDS="$PIDS $!"
sleep 1.5

echo ">> linking mic → $NODE (WirePlumber rule on the production image replaces this)"
CAP_L=$(pw-link -o | grep -E 'capture_FL$' | head -1 || true)
CAP_R=$(pw-link -o | grep -E 'capture_FR$' | head -1 || true)
[ -n "$CAP_L" ] && pw-link "$CAP_L" "$NODE:in_0" 2>/dev/null || echo "   (no capture_FL — link $NODE:in_0 manually)"
[ -n "$CAP_R" ] && pw-link "$CAP_R" "$NODE:in_1" 2>/dev/null || true

echo ">> supervisor (FSM)"
"$BIN/hermes_supervisor" & PIDS="$PIDS $!"

if [ -x "$BIN/hermes_voice_trigger" ]; then
    echo ">> voice trigger (KWD 'hey aria' + preroll ring)"
    "$BIN/hermes_voice_trigger" & PIDS="$PIDS $!"
else
    echo "!! no hermes_voice_trigger — trigger turns via the GUI (session_start) instead"
fi

if [ -x "$BIN/hermes_story_agent" ]; then
    echo ">> story agent (STT_FINAL consumer)"
    "$BIN/hermes_story_agent" & PIDS="$PIDS $!"
fi

echo ">> llm_connector — capture from $NODE:out_0 (clean feed), resident STT, Groq"
HERMES_PW_CAP_TARGET="$NODE" "$BIN/hermes_llm_connector" & PIDS="$PIDS $!"

sleep 1
echo ">> graph links through the engine:"
pw-link -l 2>/dev/null | grep -i hermes -A1 || true
echo ">> voice path is up — say 'hey aria, <question>'.  Ctrl-C to stop."
wait
