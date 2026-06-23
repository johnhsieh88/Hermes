# Hermes — Embedded LLM-Interactive AudioBox

Two-plane architecture (design: local `AudioBox_DSP_Framework_SDS.md`):

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
            cloud_connector/ on-target proxy: PipeWire client <-> network STT/LLM/TTS
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
