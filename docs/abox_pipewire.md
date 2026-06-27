l# Hermes — single-diagram view: abox_node ↔ PipeWire ↔ control plane

Status legend:  `✅` built & tested · `⚠` partial / stub body · `⛔` missing or not wired

```
══════════════════════════════ CONTROL PLANE (C++, non-RT) ══════════════════════════════
  ┌───────────┐   ┌───────────┐   ┌──────────────┐   ┌──────────┐   ┌──────────┐
  │ VTS / KWD │   │ SUPERVISOR│   │CLOUD_CONNECTOR│  │ CODEC_HW │   │VIDEO_PROC│
  │  ⛔ stub  │   │ FSM  ✅   │   │  ⛔ stub      │  │ ⛔ stub  │   │ ⛔ stub  │
  └─────┬─────┘   └─────┬─────┘   └──────┬───────┘   └────┬─────┘   └────┬─────┘
        │ WAKE_CONFIRMED│ SET_MODE/verbs  │ STT/TTS        │ reset/btn    │
        └───────────────┴────────┬────────┴────────────────┴──────────────┘
                                 │   MsgBus over POSIX mq  ✅  (URGENT/NORMAL/DEFERRED lanes)
                                 │   pre-roll ring (VTS→ABOX) ⛔ defined, not wired
                                 ▼
                       ┌───────────────────────┐
                       │ AudioCore handler ⚠   │  SET_MODE→mode ✅ ; START_CAPTURE/DUCK/
                       │ (abox_main.cpp)       │  ARM_BARGE_IN/RESET ⛔ TODO
                       └───────────┬───────────┘
                                   │ atomic mode swap (ModeControl / param_store ✅)
═══════════════════════════════════│════════ PIPEWIRE DOMAIN (RT data-loop, A76 cpu4) ════
                                   │
   ┌───────────────────────────────▼──────────────────────────────────────────────┐
   │ PipeWire (libpipewire-0.3)  ✅   clock(spa_io_position) · ports · WirePlumber  │
   │   mic capture ─► [ on_process(pos) per 5ms quantum ] ─► sink ─► DAC/smart-amp   │
   │   reported latency (SPA_PARAM_Latency) ──► seeds AEC bulk delay                 │
   └───────────────────────────────┬──────────────────────────────────────────────┘
                                   │
        ⛔ D9 GAP: the live on_process bridges to the C++ dsp::Node (PwStage.cpp),
                   NOT to the C abox_node data plane below. The C path is offline-only.
                                   │
══════════════════════════════════▼══ BRIDGE + COORDINATOR  (Model B, buffer_pipeline.c) ══
   ┌──────────────────────────────────────────────────────────────────────────────┐
   │ hermes_pipeline_process_tick()  ✅(built, ⚠ not a live filter)                  │
   │                                                                                │
   │  CORE-PROPORTIONAL BUFFER POOL ✅           (period-level / pipeline parallelism)│
   │   slot0(cpu5) abox_frame ─ _Atomic in_progress[0]                              │
   │   slot1(cpu6) abox_frame ─ _Atomic in_progress[1]   firewall: busy?─►SOFT-DROP  │
   │   slot2(cpu7) abox_frame ─ _Atomic in_progress[2]   ⚠ sync=detect, async=absorb │
   │   pick slot → ZERO-COPY alias pw buffer → rotate ──────────────┐                │
   └───────────────────────────────────────────────────────────────│────────────────┘
                                                                    │ abox_frame
══════════════════════════════ DATA PLANE (pure C, no pw_*) ═══════▼══════════════════════
   ┌──────────────────────────────────────────────────────────────────────────────┐
   │ abox_graph_tick()  ✅   mask = abox_active_mask(mode)  → (mask&bit)?process:SKIP │
   │                                                                                │
   │   ┌────────┐    ┌────────┐      ┌──────────┐     ┌────────┐                     │
   │   │ SRC ✅ │──► │ AEC ⚠  │ ───► │ BEAM ⚠   │ ──► │ SES ⛔ │ ──► egress copy ►out │
   │   │ ASRC   │ 2ch│ ramp + │  2ch │ avg 2→1  │ 1ch │ passthr│                     │
   │   │ resamp │    │ ref ✅ │      │ (no MVDR)│     │ (stub) │                     │
   │   └────────┘    └───┬────┘      └──────────┘     └────────┘                     │
   │     each node = nodes/*.c, vtable ops->process, sees ONLY abox_frame            │
   │                     │ aligned far-end                                           │
   │         ┌───────────▼────────────┐      ┌──────────────────────────────────┐    │
   │         │ RefManager ✅ §4.3.2    │      │ WORKER POOL ✅ §11.2 (cpu5..7)   │    │
   │         │ ring+VI-Sense+bulk delay│      │ abox_cmd · done_ctr join         │    │
   │         │ ⛔ no xcorr delay-lock  │◄─────│ (within-block data parallelism)  │    │
   │         │ ⛔ no §5 drift PI loop  │      │ ⚠ not invoked by any live node   │    │
   │         └─────────────────────────┘      └──────────────────────────────────┘    │
   └──────────────────────────────────────────────────────────────────────────────┘
                                   │
        ⛔ MISSING: VAD node that detects local speech and EMITS AUDIO_CORE::evt::BARGE_IN
                   (FSM has the handler; nothing sends the event → barge-in can't fire)
                                   ▼
                       overrun on deadline? → abox_soft_mute ✅ (zero-fill, §6.1)
```

## What is missing — implementation vs. wiring

**Not wired (plumbing exists, not connected):**
- `⛔ D9` — live `on_process` runs the **C++** `dsp::Node`, not the C `abox_node`s (needs a C `pw_filter` bridge, Model A, *or* `buffer_pipeline` as the filter, Model B).
- `⚠` Buffer pool runs **synchronous** → firewall *detects* overruns but doesn't *absorb* them; needs the async pool-dispatch + previous-slot egress collect.
- `⚠` Worker pool is built/tested but **no live node fans out** into it yet (AEC/MVDR will).
- `⛔` Pre-roll ring defined but not connected between VTS and ABOX.
- `⛔` ABOX control verbs (START_CAPTURE / DUCK_PLAYBACK / ARM_BARGE_IN / RESET_PIPELINE) are TODO in `abox_main.cpp`.

**Not implemented (algorithm / logic bodies):**
- `⚠ AEC` — framework + ramp + reference pull real; **PBFDAF kernel + DTD = TODO**.
- `⚠ BEAM` — naïve 2→1 average; **MVDR/GSC steering = TODO**.
- `⛔ SES` — passthrough; spectral suppression = TODO.
- `⛔ VAD / barge-in emitter` — nothing detects local speech or sends `BARGE_IN`.
- `⛔ §5 1 ms scheduler` — clock-drift PI (feeds `abox_src_set_ratio`), volume fader, `/dev/input`.
- `⛔ RefManager delay-lock` — bulk delay is seed-only; no cross-correlation re-lock.
- `⛔ Cloud connector / KWD / codec` process bodies (control-plane stubs).

**Done & tested:** MsgBus mq transport ✅, mask-gated graph ✅, routing/param ✅, buffer pool + firewall ✅, worker pool ✅, RefManager ring ✅, SRC ASRC ✅, Soft-Mute ✅, Supervisor FSM ✅ — all green in `abox_selftest` + `test_msgbus` (10/10), and cross-compiled for RK3588.
