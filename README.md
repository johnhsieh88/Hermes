# Hermes — Embedded LLM-Interactive AudioBox

Two-plane architecture (full design: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — the AudioBox DSP Framework SDS is **Part II** of that doc):

- **Data plane = PipeWire** (on target): mic, AEC reference, clean mono, TTS playback — zero-copy, clocked by the 5 ms quantum.
- **Control plane = MsgBus** (POSIX mq per `ModuleId`): every command/event, async, priority-laned.

## Layout
```
common/   libhermes_common — the IPC contract (CMsg, ModuleId, Catalog, EventMap, MsgBus, Transport, PrerollRing)
app/      the 6 independent processes:
            supervisor/      Session FSM orchestrator (SDS §15)
            audio_core/      ABOX — DSP RT island as a PipeWire SPA node (SDS §14.4) + barge-in (§8)
            voice_trigger/   VTS — always-on keyword detection, own mic tap (SDS §16)
            video_proc/      A/V sync
            llm_connector/   on-target proxy: PipeWire client <-> network STT/LLM/TTS
            codec_hw/        I2C codec + buttons
test/     unit/ + integration/ (barge_in_e2e, kwd_wake_e2e are the primary KPIs, SDS §17.2)
```

## The two key paths (SDS §17.2)
- **Barge-in** — ABOX RT island (hard-RT, AEC residual) → `BARGE_IN` (URGENT lane), duck ≤ 12 ms.
- **Keyword detection** — VTS (own process, own raw mic tap) → `WAKE_CONFIRMED`; zero-copy pre-roll ring (`/hermes.preroll`) so the command is never clipped.

VTS and ABOX are **separate processes**, each capturing the mic independently; they meet only at the `WAKE_CONFIRMED` interrupt (control) and the `PrerollRing` (one-shot data handoff).

## Build
```
cmake -S . -B build && cmake --build build
ctest --test-dir build
```

## Status

### Phase 3 — Full Voice Loop (verified 2026-07-11)

The complete dialog cycle runs end-to-end on QEMU aarch64 (cortexa57 sysroot cross-compile,
`hermes_llm_connector` built from `llm_connector/main.cpp`):

```
SS_INIT → SS_IDLE → SS_CAPTURE → SS_THINK → SS_SPEAK → SS_IDLE
```

**Verified on QEMU (qemuarm64, SSH port 2222):**

| Stage | Component | Result |
|---|---|---|
| WAKE_CONFIRMED → SS_IDLE | Supervisor FSM auto-transition at boot | ✓ |
| OPEN_STREAM → capturing | LLM_CONNECTOR VAD + PipeWire snd_dummy (1289+ callbacks/20s) | ✓ |
| VAD silence timeout → STT_ENDPOINT | kMaxUtterMs=20s path (321k samples captured) | ✓ |
| STT_NO_SPEECH → SS_IDLE | Supervisor recovery handler | ✓ |
| STT (stub) → Groq API → reply | Real HTTP call, `llama-3.1-8b-instant`, live GROQ_API_KEY | ✓ |
| TTS (stub) → TTS_CHUNK → SS_SPEAK | Supervisor SS_THINK → SS_SPEAK | ✓ |
| PLAYBACK_DRAINED → SS_IDLE | Full loop completion | ✓ |

**Actual logs captured during test (2026-07-11):**
```
[SUP] state INIT → IDLE
[SUP] WAKE_CONFIRMED received (state=IDLE)
[SUP] startTurn: DISARM→VTS, OPEN_STREAM→LLM, START_CAPTURE→AC
[SUP] state IDLE → CAPTURE
[CC] pipeline: pcmBuf=321536 samples (20.1s), abort=0
[CC] STT stub: 'hello aria'
[CC] transcript: 'hello aria'
[CC] reply: *Konnichiwa!* Welcome back to my corner of the desk. How may I brighten your day today?
[CC] TTS stub: generating 0.5s silence for '...'
[SUP] state CAPTURE → THINK
[SUP] state THINK → SPEAK
[SUP] state SPEAK → IDLE
```

**Known QEMU limitations (not code bugs):**
- sherpa-onnx STT inference takes minutes on emulated aarch64 — use `HERMES_TEST_UTTERANCE` stub
- Piper TTS ONNX inference is heavy enough to crash QEMU — use `HERMES_TEST_UTTERANCE` stub
- virtio ALSA card (qemuarm64) is suspended — set `HERMES_PW_CAP_TARGET` / `HERMES_PW_PLAY_TARGET` to `alsa_*_snd_dummy.*` (see QEMU test section in BUILD.md)

**On real hardware** (cortexa57/RK3588): do not set stub env vars; WirePlumber auto-connects to the codec.

### Environment variables

| Variable | Purpose |
|---|---|
| `HERMES_TEST_UTTERANCE=<text>` | Skip sherpa-onnx + Piper; inject hardcoded transcript + 0.5s silence TTS |
| `HERMES_PW_CAP_TARGET=<node>` | Override WirePlumber auto-connect for capture (QEMU: `alsa_input.platform-snd_dummy.0.stereo-fallback`) |
| `HERMES_PW_PLAY_TARGET=<node>` | Override WirePlumber auto-connect for playback (QEMU: `alsa_output.platform-snd_dummy.0.stereo-fallback`) |
