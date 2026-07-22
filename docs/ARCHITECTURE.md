# Hermes — Architecture & Design

**Product:** Interactive Multi-Character Audiobook on the Hermes Embedded AudioBox
**System class:** Embedded real-time multimedia system (heterogeneous multicore SoC)
**Target:** Rockchip RK3588 · aarch64 / ARMv8.2-A · PipeWire on PREEMPT_RT Linux
**Document status:** Draft v0.3 · 2026-07-19 · *living document — update alongside the code*
*(v0.3: SES removed from the cascade; BEAM made channel-preserving 2→2; DMX 2→1 added as the
final structural stage; `out_0` contract fixed at mono/f32/48 kHz — see §13.2 design-change notes.
2026-07-20: CAPGATE structural gate built (`START/STOP_CAPTURE` live); UC-12 capture→STT→cloud
path code-complete incl. preroll backfill; §13.2.1 Node Reference, §16.7 data flow, §16.8 UC-12
added; `Log.h` logging standard; `run_voice.sh` launcher.)*
**Audience:** Engineering, product, security/privacy review

> This is the **single authoritative architecture & design document** for Hermes. It spans
> product framing through low-level engineering contracts, and is grounded in the **implemented
> codebase** (exact enum values, struct layouts, FSM transitions, file:line). Every element is
> tagged **built ✅ / framework ⚠ / planned ⛔** so the document stays honest against the source.
>
> **This file is the consolidated grand doc — it absorbs three formerly-separate documents:**
> **Part I** — System & Product Architecture (this part, verified against code);
> **Part II** — the AudioBox DSP Framework SDS (deep engine design, folded in from
> `AudioBox_DSP_Framework_SDS.md`);
> **Part III** — abox↔PipeWire architecture & per-mode call sequences (folded in from
> `abox_pipewire.md`). Both source files have been removed; nothing was dropped. Where Parts II/III
> overlap Part I, **Part I is authoritative** (see the precedence note opening Part II).
> Remaining companion specs: `memory_architecture.md`, `story_agent_SDS.md`, `VALIDATION.md`,
> `SVVR.md` (Software Verification & Validation Report).

---

## Table of Contents

**Three parts** (each with its own internal §-numbering):
- **Part I — System & Product Architecture** — §1–§24 below (code-verified; authoritative on overlaps)
- **Part II — AudioBox DSP Framework SDS** — Part II §1–§18 (deep DSP engine internals)
- **Part III — abox↔PipeWire Call Sequences** — Part III §1–§7 (per-mode as-built sequences)

### Part I
1. Purpose & Scope
2. Product Overview
3. Architectural Drivers & Principles
4. System Context & Actors
5. Use Cases
6. Functional Requirements
7. Non-Functional Requirements / KPIs
8. Target Hardware Specification
9. System Architecture Overview
10. Process / Module Inventory
11. Control-Plane IPC
12. Message Contract Catalog
13. Audio Data Plane (ABOX + PipeWire)
14. State Machines
15. High-Level Call Sequences
16. Low-Level Detailed Sequences (Playback §16.5–16.6 · End-to-End Data Flow §16.7 · UC-12 Audio Capture §16.8)
17. Knowledge & Memory Subsystem
18. Cross-Cutting Concerns
19. Deployment & Build
20. Technology Choices & Rationale
21. Implementation Status
22. Limitations, Risks & Open Decisions
23. Phased Roadmap
24. Glossary & References

---

# Part I — System & Product Architecture

> Code-verified system and product architecture. Authoritative on the command/event catalog,
> ModuleId addressing, Session-FSM states, and engine modes wherever Parts II/III differ.

## 1. Purpose & Scope

### 1.1 Purpose
Define the end-to-end architecture for a small-board device that **reads books aloud with distinct
character voices and emotion**, and lets a listener (a child) **talk to the story** — ask
questions, interrupt, be remembered across sessions — entirely **local-first and private**. The
document specifies the hard-real-time audio data plane, the asynchronous control plane, the
message contract between processes, the session and audio state machines, the knowledge/memory
subsystem, and the safety, privacy, and deployment model. It is detailed enough to implement
against and is the single source of truth for cross-module contracts.

### 1.2 Scope
**In scope:** device software architecture, IPC contract (MsgBus/CMsg/EventMap), ABOX DSP island
and PipeWire hosting, control-plane orchestration (SessionFsm, story_agent, llm_connector), the
knowledge & memory subsystem, use cases, functional/non-functional requirements, target hardware,
call sequences, state machines, safety/privacy, deployment.
**Out of scope:** hardware/electrical (PCB) design, cloud billing, the book content catalog itself.

---

## 2. Product Overview

| Capability | Description |
|------------|-------------|
| Multi-voice narration | Each character is cast to a distinct voice; lines carry an emotion tag |
| Interactive | Wake word / barge-in → ask the narrator or a character a question, then resume |
| Memory | Remembers the listener (name, level, favorites, progress) and story facts across sessions |
| Local-first | Recall + playback work offline; data stays on the device; cloud is opt-in |
| Child-safe | Guardrails on every generated response; parent-auditable memory |

---

## 3. Architectural Drivers & Principles

1. **Two-plane separation.** A hard-real-time **data plane** (PipeWire/ABOX, 5 ms quantum) is
   isolated from an asynchronous **control plane** (POSIX-mq MsgBus). Long-latency AI never runs
   on the audio island.
2. **Local-first, cloud-optional.** Default to on-device; cloud is an *enhancement* (heavy
   extraction, backup, parent dashboard, sync) gated by explicit consent — never a hard
   dependency. Rationale: child-data regulation (COPPA/GDPR-K), offline use, latency, cost.
3. **Everything readable.** Curated knowledge, persona, guardrails, casting, and the exported
   memory snapshot are **markdown in the Open Knowledge Format (OKF)** — human- and agent-readable,
   version-controllable, viewable with OKF's offline HTML visualizer.
4. **Swappable seams.** Memory is reached through a 3-call facade (`recall`/`remember`/`export`);
   LLM/TTS through the connector. Implementations (local search ↔ mem0; local LLM ↔ cloud) swap
   behind these contracts without caller changes.
5. **Process isolation = blast-radius control.** Each responsibility is its own process bound to
   the bus; a stalled network/AI task degrades one feature, never the audio loop.
6. **Honest degradation.** Missing sidecar → empty memory; offline → cached playback + local
   recall; slow DSP block → one Soft-Mute, not an ALSA Xrun.

---

## 4. System Context & Actors

```
                 ┌──────────────┐        speaks / listens        ┌──────────────┐
                 │   Listener   │  ◄───────────────────────────► │   Hermes     │
                 │   (child)    │     mic in / speaker out       │   AudioBox   │
                 └──────────────┘                                │  (RK3588)    │
                 ┌──────────────┐    views/erases memory,        │              │
                 │    Parent    │  ◄── settings, consent ──────► │              │
                 └──────────────┘                                └──────┬───────┘
   OPTIONAL / consented, online-only:                                   │
   ┌───────────────┐  ┌───────────────┐  ┌───────────────┐             │
   │ Cloud LLM     │  │ Cloud TTS      │  │ Backup +       │ ◄───────────┘
   │ (complex Q&A) │  │ (book prerender)│  │ Parent portal  │
   └───────────────┘  └───────────────┘  └───────────────┘
```

| Actor | Type | Interaction |
|-------|------|-------------|
| **Listener (child)** | Primary human | Speaks/listens; wake word, barge-in questions, hears narration |
| **Parent/Guardian** | Human | Consent, settings, views/erases memory (OKF visualizer), privacy controls |
| **Book content** | External data | Pre-rendered audio + per-book OKF facts loaded onto the device |
| **Cloud LLM** | Optional external system | Complex Q&A when consented + online |
| **Cloud TTS** | Optional external system | Offline book pre-render (expressive voices) |
| **Backup / Parent portal** | Optional external system | Encrypted backup, dashboard, multi-device sync |
| **mem0 sidecar** | Local co-process | Vector recall/extraction over HTTP (localhost) |

---

## 5. Use Cases

| ID | Use case | Status |
|----|----------|:---:|
| **UC-1** | **Narration** — read a multi-character book aloud with per-character voice + emotion | ⚠ framework |
| **UC-2** | **Wake-word interactive turn** — wake word, ask a question, hear a spoken answer, narration resumes | ⚠ framework |
| **UC-3** | **Barge-in interactive turn** — interrupt mid-narration; playback ducks, question answered, resume | ⚠ FSM orchestration built; duck is ABOX-local; ⛔ no VAD producer (dormant end-to-end) |
| **UC-4** | **Memory recall** — answer reflects remembered listener facts (name, level, favorites, progress) | ✅ facade + sidecar; ⛔ local OKF |
| **UC-5** | **Memory consolidation ("sleep")** — at idle, distill episodic logs → durable OKF facts | ⛔ planned |
| **UC-6** | **Parent review/erase** — view/erase the child's memory | ⛔ planned (`exportMd` stub) |
| **UC-7** | **Book preparation (offline batch)** — text → speaker/emotion tag → cast → pre-render audio → OKF | ⛔ planned |
| **UC-8** | **Push-to-talk session** — button/`START_SESSION` skips wake word | ⛔ catalog id exists; supervisor handler is TODO (not wired) |
| **UC-9** | **Fault recovery** — codec unplug / pipeline error → reset & recover | ✅ FSM `SS_FAULT` |
| **UC-10** | **Privacy mute** — hardware mute button cuts capture | ⛔ stub (`BUTTON_MUTE` defined) |
| **UC-11** | **Dev/test control** — web GUI drives modes/volume/barge-in onto the bus | ✅ `gui_interface` |
| **UC-12** | **Audio Capture** — mic → abox clean `out_0` → on-device STT → cloud LLM → spoken answer (§16.8) | ⚠ capture+resident-STT built (v0.3 slice); answer playback still bypasses abox |

---

## 6. Functional Requirements (FR)

| ID | Requirement | Status |
|----|-------------|:---:|
| FR-1 | Read a loaded script segment-by-segment with per-segment speaker + emotion casting | ⚠ |
| FR-2 | Detect wake word (own mic tap) and start an interactive turn | ⛔ (VTS stub) |
| FR-3 | Duck/mute narration on barge-in within the latency budget | ⚠ ABOX-local duck designed; ⛔ no VAD trigger |
| FR-4 | Transcribe listener speech (STT), route to a local or cloud LLM, synthesize expressive TTS | ⚠ skeleton |
| FR-5 | Apply guardrails to every generated response before TTS | ⛔ output gate planned |
| FR-6 | Recall top-k listener/story facts before answering | ✅ facade→sidecar |
| FR-7 | Append each turn to an episodic log; consolidate to durable facts at idle | ⚠ remember ✅, consolidate ⛔ |
| FR-8 | Resume narration at the correct position after an interaction | ✅ position pointer |
| FR-9 | Operate fully offline (cached playback + local recall) | ⚠ depends on local backend |
| FR-10 | Parent can view and erase memory | ⛔ |
| FR-11 | Recover from codec/pipeline faults without a full restart | ✅ |

## 7. Non-Functional Requirements (NFR / KPIs)

| ID | Attribute | Target | Status |
|----|-----------|--------|:---:|
| NFR-1 | RT audio quantum | 5 ms / 240 frames @ 48 kHz, lock-quantum | ✅ enforced |
| NFR-2 | IPC control latency | ≤ 2 ms (one mq hop) | ✅ plausible |
| NFR-3 | Barge-in ducking | ≤ 12 ms | ⛔ blocked on VAD node |
| NFR-4 | Local memory recall | ≤ 150 ms p95 (local OKF) / ≤ 350 ms (sidecar) | ⚠ unverified |
| NFR-5 | Memory write (consolidation) | off-turn, seconds OK | ✅ by design |
| NFR-6 | Memory footprint | < ~150 MB RAM | ⚠ target |
| NFR-7 | Offline operation | full playback + recall | ⚠ |
| NFR-8 | No allocation / no blocking on RT path | hard | ✅ |
| NFR-9 | Encryption at rest; parental consent for any cloud egress | hard | ⛔ to wire |
| NFR-10 | Graceful degradation (sidecar/cloud absent) | always | ✅ |

---

## 8. Target Hardware Specification

| Item | Spec | Source |
|------|------|--------|
| SoC | Rockchip RK3588 | `cmake/rk3588.toolchain.cmake` |
| ISA | aarch64 / ARMv8.2-A (NEON) | toolchain `-mcpu=cortex-a76.cortex-a55` |
| Big cores | 4× Cortex-A76 @ ~2.4 GHz — **CPU4–7**, DSP/RT island | `BUILD.md` |
| LITTLE cores | 4× Cortex-A55 @ ~1.8 GHz — **CPU0–3**, control + ALSA/I2S IRQ | `BUILD.md` |
| RT isolation | `isolcpus=4-7 nohz_full=4-7 rcu_nocbs=4-7 irqaffinity=0-3` | SDS kernel cmdline |
| DSP worker cores | 2 reserved A76 slots (default first_core=5 → cpu5,cpu6), SCHED_FIFO 89 | `buffer_pipeline.h:23`, `worker_pool.c:60`, `abox_main.cpp:107` |
| Audio I/O | 2-ch mic in (`in_0`,`in_1`), 1-ch beamformed out (`out_0`); I2S/TDM via I2C codec | `Pw.cpp:107`, `codec_hw` |
| Sample format | 32-bit float mono per port, 48 kHz fixed | `Pw.cpp:91`, `abox_main.cpp:116` |
| RAM target | ~4 GB typical; memory subsystem budget < ~150 MB | `memory_architecture.md` |
| Storage | encrypted-at-rest `/var/lib/hermes/` | `memory_architecture.md` |
| OS | Linux + PipeWire 0.3 + WirePlumber, PREEMPT_RT | `app/CMakeLists.txt:28` |

Dev/test: arm64 containers on macOS; loopback via PipeWire null sinks (no hardware).

---

## 9. System Architecture Overview

One process per responsibility, each linking `libhermes_common` (CMsg/MsgBus/EventMap) and
addressed by **ModuleId**. Audio is the only data-plane process.

```
══════════ DATA PLANE (PipeWire, zero-copy, RT, A76 cores) ══════════
  mic ─in_0/in_1─► [hermes.abox filter] ─out_0─► speaker
                    SRC → AEC → BEAM → DMX → CAPGATE   (mode-gated cascade, 240-frame quantum)
                    vDMA ingress → buffer-pool slot[i] → worker[i]@cpu(5+i) → vDMA egress

══════════ CONTROL PLANE (POSIX-mq MsgBus, async, A55 cores) ══════════
  supervisor(1) ── SessionFsm: INIT→IDLE→CAPTURE→THINK→SPEAK→(BARGE_DUCK)→… FAULT
      │  _AudioCore::cmd SET_MODE/START_CAPTURE…   ▲ _AudioCore::evt, _Llm::evt, _VoiceTrigger::evt
      │  _Story::cmd START/PAUSE/RESUME            │
      ▼                                            │
  story_agent(8) ── script, position, casting, Memory facade, barge-in Q&A
      │  _Llm::cmd PLAY_SEGMENT(idx)               ▲ _Llm::evt TTS_STREAM_END / STT_FINAL
      ▼                                            │
  llm_connector(5) ── STT · LLM route(local|cloud) · expressive TTS · guardrail gate
  voice_trigger(3): own mic tap, wake word    codec_hw(6): I2C codec + buttons
  video_proc(4): A/V sync (future)            gui_interface(7): host-only test bridge

══════════ KNOWLEDGE & MEMORY (local-first) ══════════
  OKF store (brain/*.md)  ◄─ story_agent reads/writes ─►  local index  | mem0 sidecar (HTTP :7070)
```

---

## 10. Process / Module Inventory

Each control-plane process links `libhermes_common` (MsgBus + CMsg + EventMap + Catalog) and is
addressed by **ModuleId** (`ModuleId.hpp`, `app/CMakeLists.txt`). Audio is the only data-plane
process.

| Process (binary) | ModuleId | Lang | Responsibility | Status |
|------------------|:---:|:---:|----------------|:---:|
| `hermes_supervisor` | 1 | C++ | Session FSM, mode policy, fault recovery, consolidation trigger | ✅ FSM |
| `hermes_abox` | 2 | C++/C | DSP RT island, PipeWire node, ducking, soft-mute | ✅ engine / ⚠ DSP kernels stubbed |
| `hermes_voice_trigger` | 3 | C++ | Always-on wake word (sherpa-onnx KWS), own mic tap, pre-roll writer | ✅ KWD |
| `hermes_video_proc` | 4 | C++ | A/V sync (future) | ⛔ stub |
| `hermes_llm_connector` | 5 | C++ | resident streaming STT (out_0 feed) · Groq LLM · Piper TTS · guardrail gate ⛔ | ⚠ voice loop real; `PLAY_SEGMENT` ⛔ |
| `hermes_codec_hw` | 6 | C++ | I2C codec + `/dev/input` buttons | ⛔ stub |
| `hermes_gui_interface` | 7 | C++ | Dev/test web bridge (HTTP → control CMsg); not on device | ✅ test tool |
| `hermes_story_agent` | 8 | C++ | Audiobook orchestration + Memory facade | ⚠ basic |
| `libhermes_common` | — | C++ | IPC contract (MsgBus/CMsg/EventMap/Catalog/EventQueue) | ✅ |
| `libhermes_abox_c` | — | C | RT engine: graph, routing, vDMA, buffer/worker pool, ref mgr | ✅ |
| `services/memory/server.py` | sidecar | Py | mem0 HTTP recall/extraction (localhost :7070) | ✅ |

Launch (no init/systemd yet — script-driven): `scripts/run_gui.sh` (PipeWire + abox + gui),
`scripts/run_target.sh` (on-device loopback), `scripts/run_voice.sh` (full UC-12 voice path:
abox+links+FSM+VTS+connector, validation checklist in its header), `scripts/run_loopback.sh`
(Mac null-sink loopback),
`scripts/build.sh` (docker cross/native/run/test).

---

## 11. Control-Plane IPC

### 11.1 Transport & addressing
- **Transport:** one **POSIX message queue per module**, named `/hermes.mod.<id>` — ModuleId *is*
  the address; there is no in-process pointer registry (can't route across separate processes).
  (`MsgBus.hpp`.)
- **Priority lanes:** `PRIO_URGENT=0, PRIO_NORMAL=1, PRIO_DEFERRED=2` (`CMsg.hpp:11`), mapped onto
  mq priority and the in-process `EventQueue` lanes. Barge-in travels **URGENT**.
- **Fully async:** no blocking request/reply. A `QUERY_STATE` is answered by an ordinary event;
  nothing blocks on a reply (`MsgBus.hpp:49`).
- **Queue limits:** `kMqMsgSize = sizeof(CMsgHead)+256` (header + 256 B inline body),
  `kMqMaxMsg = 10` depth (unprivileged mq limit), `kMaxQueueDepth = 4096` (`MsgBus.hpp:64-66`).

### 11.2 Wire message — `CMsg`
Fixed **20-byte header** + inline POD body (`CMsg.hpp`, `static_assert(sizeof(CMsgHead)==20)`).
*(The legacy "32-byte" comment in the header is stale; the static_assert is authoritative.)*

```c
#pragma pack(push, 4)
struct CMsgHead {
    uint16_t version;     // PROTOCOL_VERSION = 1
    uint16_t id;          // Catalog key — encodes owner module + cmd/evt
    uint8_t  src;         // ModuleId of sender
    uint8_t  dest;        // ModuleId of recipient
    uint8_t  prio;        // TriggerPrio lane (→ mq priority)
    uint8_t  flags;       // reserved
    uint32_t length;      // inline body byte length (0 if none)
    uint64_t timestampNs; // PipeWire clock domain (spa_io_position.clock.nsec)
};
#pragma pack(pop)
struct CMsg { CMsgHead hdr; void* pBody; };   // pBody → inline body in the recv buffer
```

**Bulk rule:** bodies are tiny POD (≤ 256 B). Bulk audio/text never travels inline — audio uses
**PrerollRing/PipeWire**; story content is referenced by **index** (`StorySegmentRef`) and the
receiver resolves it against the cached script / pre-rendered clip (`CMsg.hpp:20`, `StoryMsg.hpp`).

### 11.3 MsgBus API (`MsgBus.hpp`)
A module subclasses `MsgBus` (transport) and overrides `ProcessMsg()` (usually
`return Execute(id, m)`), composing with `EventMap` (dispatch).
- Lifecycle: `ConnectMsg(sysId, recvTask=true)` opens own mq + spawns a recv thread;
  `DisconnectMsg()`.
- Send: `SendMsg(CMsgHead*, dest, flowctl)`; convenience `SendMsg(dest, id, prio, body, len)`;
  `TrySendMsgBestEffort()` (never blocks — drop on full).
- Receive: `RecvMsg(CMsg*)` (blocking) → `RecvMsgTask()` loop → `ProcessMsg()` → `FlushMsg()`.
- Introspection: `GetQ/SetQ/ConfigMsgQ`, `PendingQueueDepth()` (mq_curmsgs), `DumpMsg()`.

### 11.4 Dispatch — `EventMap<T>` (`EventMap.hpp`)
A per-module `std::map<uint16_t id, void(T::*)(const CMsg*)>`. `Add(id, &T::handler)` registers;
`Execute(id, m)` invokes the bound member (returns 1 if handled, 0 if not). A module =
`MsgBus` + `EventMap<Self>`.

### 11.5 In-process fan-in — `EventQueue` (`EventQueue.hpp`)
Multi-producer (URGENT recv thread + worker-pool results) → **single consumer** (FSM thread).
Three lanes drained **URGENT → NORMAL → DEFERRED**. `StoredMsg` copies header + ≤256 B body so the
FIFO owns its data. mutex/condvar are acceptable here — this is the **non-RT** control plane.

---

## 12. Message Contract Catalog

IDs are derived from `ModuleId.hpp`: `HM_CMD_FIRST(m) = m*0x100` (commands, base..base+0x7F),
`HM_EVT_FIRST(m) = m*0x100 + 0x80` (events). **IDs are stable — append within a block, never
renumber, reserve removed ids.** (`Catalog.hpp`.) Low 128 of a module's block = commands TO it;
high 128 = events FROM it.

### 12.1 SUPERVISOR (1)
| Dir | Name | ID | Notes |
|-----|------|----|-------|
| cmd | SHUTDOWN | 0x100 | |
| cmd | SET_MODE_POLICY | 0x101 | |
| cmd | START_SESSION | 0x102 | PTT — skip wake word (UC-8) |
| cmd | CANCEL_SESSION | 0x103 | |
| cmd | FACTORY_RESET | 0x104 | |
| cmd | QUERY_STATE | 0x105 | reply as async event |
| evt | STATE_CHANGED | 0x180 | broadcast on every FSM `enter()` |
| evt | SESSION_STARTED | 0x181 | |
| evt | SESSION_ENDED | 0x182 | |
| evt | FAULT | 0x183 | |
| evt | READY | 0x184 | |
| internal | TO_NO_SPEECH | 0x191 | FSM-private timer, not on wire |
| internal | TO_RESPONSE | 0x192 | " |
| internal | TO_SESSION_MAX | 0x193 | " |
| internal | TO_RESET | 0x194 | " |

### 12.2 AUDIO_CORE / ABOX (2)
| Dir | Name | ID | Notes |
|-----|------|----|-------|
| cmd | SET_MODE | 0x200 | EngineMode (abox_mode) |
| cmd | START_CAPTURE | 0x201 | |
| cmd | STOP_CAPTURE | 0x202 | |
| cmd | PLAY_TTS | 0x203 | |
| cmd | STOP_TTS | 0x204 | |
| cmd | DUCK_PLAYBACK | 0x205 | barge-in duck |
| cmd | SET_VOLUME | 0x206 | |
| cmd | FREEZE_ADAPT | 0x207 | AEC DTD freeze |
| cmd | ARM_BARGE_IN | 0x208 | |
| cmd | DISARM_BARGE_IN | 0x209 | |
| cmd | RESET_PIPELINE | 0x20A | |
| cmd | REF_RELOCK | 0x20B | |
| cmd | QUERY_STATE | 0x20C | sync→async reply |
| evt | *(0x280 reserved)* | 0x280 | KWD_CANDIDATE retired (KWD lives in VTS) |
| evt | BARGE_IN | 0x281 | **KEY PATH, URGENT lane** |
| evt | VAD_SPEECH_ON | 0x282 | |
| evt | VAD_SPEECH_OFF | 0x283 | |
| evt | CAPTURE_STARTED | 0x284 | |
| evt | PLAYBACK_STARTED | 0x285 | |
| evt | PLAYBACK_DRAINED | 0x286 | gates SPEAK→IDLE |
| evt | REF_LOCKED | 0x287 | |
| evt | AEC_ERLE_DROP | 0x288 | |
| evt | SOFT_MUTE | 0x289 | deadline-miss fallback fired |
| evt | XRUN | 0x28A | |
| evt | MODE_CHANGED | 0x28B | |
| evt | CLOCK_ANCHOR | 0x28C | A/V sync |

### 12.3 VOICE_TRIGGER / VTS (3)
| Dir | Name | ID | Notes |
|-----|------|----|-------|
| cmd | ARM | 0x300 | |
| cmd | DISARM | 0x301 | |
| cmd | SET_THRESHOLD | 0x302 | |
| evt | WAKE_CONFIRMED | 0x380 | **KEY PATH** — body: WakeConfirmedBody |
| evt | WAKE_REJECTED | 0x381 | |

### 12.4 VIDEO_PROC (4)
| Dir | Name | ID |
|-----|------|----|
| cmd | SYNC_ANCHOR | 0x400 |
| cmd | START | 0x401 |
| cmd | STOP | 0x402 |
| evt | SYNC_DRIFT | 0x480 |
| evt | FRAME_DROP | 0x481 |

### 12.5 LLM_CONNECTOR (5)
| Dir | Name | ID | Notes |
|-----|------|----|-------|
| cmd | OPEN_STREAM | 0x500 | open STT session + subscribe to ABOX clean mono |
| cmd | CLOSE_STREAM | 0x501 | |
| cmd | UTTERANCE_END | 0x502 | triggers route()+inference |
| cmd | ABORT | 0x503 | barge-in cancels in-flight inference/TTS |
| cmd | PLAY_SEGMENT | 0x504 | story_agent → render seg idx (TTS); body `StorySegmentRef` |
| evt | CONNECTED | 0x580 | |
| evt | DISCONNECTED | 0x581 | |
| evt | STT_PARTIAL | 0x582 | |
| evt | STT_FINAL | 0x583 | barge-in utterance → story_agent |
| evt | STT_ENDPOINT | 0x584 | end-of-utterance → CAPTURE→THINK |
| evt | STT_NO_SPEECH | 0x585 | |
| evt | LLM_BEGIN | 0x586 | |
| evt | TTS_CHUNK | 0x587 | first chunk → THINK→SPEAK |
| evt | TTS_STREAM_END | 0x588 | segment done → story_agent advances |
| evt | LLM_ERROR | 0x589 | local or cloud |

### 12.6 CODEC_HW (6)
| Dir | Name | ID | Notes |
|-----|------|----|-------|
| cmd | RESET | 0x600 | |
| cmd | SET_GAIN | 0x601 | |
| cmd | MUTE | 0x602 | |
| cmd | UNMUTE | 0x603 | |
| evt | UNPLUGGED | 0x680 | codec fault → FSM `SS_FAULT` |
| evt | PLUGGED | 0x681 | |
| evt | OVERTEMP | 0x682 | |
| evt | READY | 0x683 | |
| evt | BUTTON_WAKE | 0x684 | PTT / action button |
| evt | BUTTON_MUTE | 0x685 | privacy mute |

### 12.7 STORY_AGENT (8)
| Dir | Name | ID | Notes |
|-----|------|----|-------|
| cmd | START | 0x800 | begin reading loaded script |
| cmd | PAUSE | 0x801 | |
| cmd | RESUME | 0x802 | |
| evt | SEGMENT_STARTED | 0x880 | body `StorySegmentRef` |
| evt | STORY_DONE | 0x881 | end of book |

### 12.8 Message bodies (POD)
| Body | Fields | Used by |
|------|--------|---------|
| `StorySegmentRef` | `int32_t segment_idx` (4 B) | `PLAY_SEGMENT`, `SEGMENT_STARTED` (`StoryMsg.hpp`) |
| `WakeConfirmedBody` | (referenced by `WAKE_CONFIRMED`) | VTS → supervisor *(struct TBD)* |
| EngineMode (u32) | `abox_mode` value | `SET_MODE` |

---

## 13. Audio Data Plane (ABOX + PipeWire)

### 13.1 PipeWire hosting (`pipewire/Pw.cpp`, `abox_main.cpp`)
- One filter **`hermes.abox`**, **2 mono DSP in** (`in_0`,`in_1`) / **1 mono DSP out** (`out_0`),
  32-bit float, 48 kHz.
- **Quantum = 240 frames (5 ms)**, locked via `node.lock-quantum=true`.
- RT callback `on_process` → `abox_block` → `hermes_pipeline_process_tick(...)`; fetches DSP
  buffers per port, calls the engine with port pointers + frame count + sample timeline. **No
  malloc, no locks** in the callback. WirePlumber links mic → `hermes.abox` → sink.

### 13.2 DSP node graph (`abox/`, `abox/nodes/`)
Static linear cascade, executed in-place per block (zero-copy):

```
SRC (2→2) → AEC (2→2) → BEAM (2→2) → DMX (2→1) → CAPGATE (1→1)
```

Every processing node is **channel-preserving** (2→2); the channel collapse happens only in the
final structural `DMX` stage. This keeps one uniform frame format through the whole cascade and
lets BEAM expose a GSC-style pair — `chan[0]` = beam (voice), `chan[1]` = blocking-matrix noise
reference — for a future post-filter, with `DMX` selecting/downmixing `chan[0]` into `out_0`.

**`out_0` contract: mono · f32 · 48 kHz — nothing else.** Consumers (STT / VAD / KWD) take it
natively; any model-rate conversion (e.g. the recognizer's 16 kHz feature frontend, or a PW
stream-adapter resample) is the **consumer's** concern. abox carries no rate conversion beyond
the head-of-cascade SRC drift trim.

> **Design change — `out_0` delivery & consumer-edge buffering (2026-07-19).** `out_0` reaches
> speech consumers over a **PipeWire link created by session policy**: the consumer's capture
> stream carries `node.target = hermes.abox` (`PW_KEY_NODE_TARGET`, `Pw.cpp:187`; env hook
> `HERMES_PW_CAP_TARGET` in `llm_connector`) and WirePlumber links `out_0 → <stream>` —
> delivery is same-cycle, zero-copy SHM. **Rate decoupling is the consumer's job**, one
> `AudioRing<N>` (`common/include/hermes/common/AudioRing.hpp` — lock-free SPSC, drop-new +
> overrun counter) per consumer: the RT stream callback only pushes; a worker thread pops
> model-sized chunks (STT ~100 ms, VAD ~32 ms). **abox never buffers for consumers and knows
> no chunk sizes** — buffering inside the engine would leak consumer pacing into the RT island
> and impose one consumer's latency on all. *Designed alternative (not built): a **TAP node** —
> an `abox_node` after DMX writing a PrerollRing-style SHM ring (`/hermes.out0`: f32,
> `writePos` + `epoch`, overwrite-oldest, position-addressed) — N readers share one ring with
> private read positions and consumers need no PipeWire; adopt when speech consumers multiply
> (ear-process migration).*

Node vtable (`abox_node.h:61`): `prepare / configure / process / reset / destroy`; `process()`
mutates one shared `abox_frame` in place. Graph storage: `abox_stage stages[ABOX_MAX_STAGES=8]`.
Limits: `ABOX_MAX_CHANNELS=2`, `ABOX_MAX_BLOCK=512`.

| Node | File | Status | Note |
|------|------|:---:|------|
| SRC | `nodes/src_node.c` | ✅ real | fractional resampler, drift, ratio clamp 0.5–2.0; identity at 1.0 |
| AEC | `nodes/aec_node.c` | ⚠ stub | pulls aligned reference, ramps blend (10 ms); PBFDAF kernel TODO — bypass today |
| BEAM | `nodes/beamform_node.c` | ⚠ stub | **2→2 (channel-preserving)**; MVDR/GSC TODO — target: chan[0]=beam, chan[1]=noise ref |
| DMX | `nodes/dmx_node.c` | ✅ | **2→1 structural downmix** — selects chan[0] → `out_0`; always on (`ABOX_ELEM_STRUCTURAL`, not mode-gated) |

`abox_selftest` ABOX-11 asserts **bit-exact `out0 == in0`** end-to-end (proves transport+graph are
lossless and BEAM passes chan[0], not an average). When real kernels land, this assertion changes.

> **Design change — SES removed (2026-07-19).** The cascade formerly ended in an `SES (1→1)`
> spectral-suppression stage (`nodes/ses_node.c`, a full-bypass stub). It is removed from the
> design: (a) no kernel was ever implemented; (b) its intended benefit — residual-echo/noise
> "polish" — serves human listeners and cloud ASR, while the on-device consumers of `out_0`
> (KWD / VAD / STT) are better served by *unpolished* post-BEAM audio: spectral suppression in
> front of models trained on raw speech typically degrades them; (c) dropping it simplifies the
> cascade and the routing matrix. If a polish stage returns, it will be an **optional element on
> a playback-/cloud-bound tap**, not in the capture spine. **Code follow-up done (2026-07-20):**
> `nodes/ses_node.c` deleted; stage wiring dropped from `abox_main.cpp`/harness/selftest; the
> `ABOX_ELEM_SES` column retired in `kGain` (slot reserved — element indices never renumber;
> Conversation mask is now `0x6F`); selftest updated and passing. Parts II/III retain SES in
> their historical text.

> **Design change — BEAM made channel-preserving; DMX added (2026-07-19).** BEAM no longer
> mutates the channel count (was 2→1 by `channels = 1`); it is now specified 2→2 so the frame
> format is uniform across all processing nodes and the GSC noise-reference channel has a home.
> The 2→1 collapse moves to a new final **DMX** stage (structural, always-on, not a matrix
> column). *(Not "SRC" — SRC is the sample-rate/drift node at the head of the cascade; a
> channel downmix is a different operation and keeps a distinct name.)* **Code follow-up done
> (2026-07-20):** `beamform_node.c` is now a pure 2→2 passthrough stub; `nodes/dmx_node.c`
> added and wired (`ABOX_ELEM_STRUCTURAL = -1` — the graph tick runs structural stages in
> every mode); `abox_selftest` all-pass, ABOX-11 unchanged in effect (`out_0 == in_0`).

#### 13.2.1 Node Reference — every element, its processing, and its I/O format

Shared frame contract for all nodes: **planar** `abox_frame` — `chan[c]` per-channel f32
pointers, `frames = 240`, `rate = 48 000`, `sample_pos` in the graph clock, processed
**in-place** (untouched frame ⇒ bit-exact bypass); `malloc` at `prepare()` only, never in
`process()`.

| Node / element | In → Out | Gating | File | Status |
|---|---|---|---|:---:|
| vDMA-IN | PW port buffers (2×mono f32) → slot frame 2 ch | structural (pipeline plumbing) | `abox_vdma.c` | ✅ |
| SRC | 2 ch → 2 ch · 48 k → 48 k (drift trim) | `ABOX_ELEM_SRC` | `nodes/src_node.c` | ✅ kernel / ⚠ no drift input |
| AEC | 2 ch → 2 ch | `ABOX_ELEM_AEC` (+ consumes REF) | `nodes/aec_node.c` | ⚠ bypass stub |
| REF | *(service, not a node)* far-end ring → AEC | `ABOX_ELEM_REF` | `reference_manager.c` | ✅ mech / ⛔ producer |
| BEAM | 2 ch → 2 ch (channel-preserving) | `ABOX_ELEM_BEAM` | `nodes/beamform_node.c` | ⚠ bypass stub |
| DMX | 2 ch → 1 ch (select chan[0]) | **structural — every mode** | `nodes/dmx_node.c` | ✅ |
| CAPGATE | 1 ch → 1 ch (capture on/off) | **structural** ×matrix gain | `nodes/capgate_node.c` | ✅ |
| TTSOUT | playback gain / duck point | `ABOX_ELEM_TTSOUT` | *(no node file)* | ⛔ |
| vDMA-OUT | slot chan[0] → `out_0` port buffer, × master gain | structural | `abox_vdma.c` (`bp_egress`) | ✅ |

**vDMA-IN — ingress (structural).** Claims the cycle's samples out of PipeWire-owned port
buffers into an engine-owned pool slot (bounded memcpy, 2×960 B) and stamps
`frames/rate/sample_pos`. Exists so the borrowed graph buffer can be returned immediately
while a worker still owns the slot — the seam that makes the async pool and the deadline
firewall possible (§13.4).

**SRC — asynchronous sample-rate converter.** Per-channel fractional linear-interp resampler
pinning the codec-crystal clock onto the graph clock: ratio ≈ 1.0 ± ppm (clamp 0.5–2.0),
phase carried across blocks, exact-identity fast path at ratio 1.0. *Must precede AEC* —
uncorrected drift slides the echo under the adaptive filter and prevents convergence. Ratio
input (`abox_src_set_ratio`, from the drift-PI estimator) is ⛔ unwired ⇒ runs at 1.0 today.
Not a channel or nominal-rate converter — 48 k in, 48 k out, always.

**AEC — acoustic echo canceller.** Design: PBFDAF (partitioned-block frequency-domain,
~190 ms tail) per channel, subtracting the time-aligned far-end (REF) from each mic; blend
ramped ~10 ms on mode entry; adaptation freezable (`FREEZE_ADAPT`, double-talk). As-built:
pulls the aligned reference and advances the ramp, then passes bit-exact — kernel TODO, and
the REF ring currently reads zeros (no live far-end producer until `in_tts` lands).

**REF — the far-end reference (element without a node).** A routing column gating the
`reference_manager` service AEC consumes: circular ring (1536 samples), integer bulk delay +
fractional sub-sample interpolation, VI-Sense tap preferred over post-fader mixer when both
exist. Producer ⛔: the target design feeds it **in-tick** from the `in_tts` playback path,
making alignment a fixed constant instead of an estimation problem.

**BEAM — spatial filter (channel-preserving, v0.3).** Design: MVDR/GSC over the two
echo-cancelled mics — `chan[0]` = the steered beam (voice), `chan[1]` = the blocking-matrix
noise reference kept for a future post-filter. As-built: pure 2→2 passthrough (frame
untouched). It no longer mutates the channel count — that was moved out on 2026-07-19.

**DMX — structural downmix (always on).** Ends the capture cascade: 2 → 1 by *selecting*
`chan[0]` (pointer bookkeeping, zero sample writes, zero state). Runs in **every** mode
(`ABOX_ELEM_STRUCTURAL`), because `out_0`'s contract (mono · f32 · 48 k, §13.2) holds
regardless of what the routing matrix is doing. A weighted downmix would live here if a
product ever wants one; a real GSC keeps `chan[1]` internal and DMX's job doesn't change.

**CAPGATE — capture gate (structural node, built 2026-07-20).** The data-plane on/off
switch for the clean feed, ending the cascade after DMX. Structural — *not* mask-gated,
because a skipped stage is a passthrough, the opposite of a closed gate; instead its gain
is `route_gain(mode, CAPGATE) × open`, so the routing matrix (idle/reset ⇒ 0 ⇒ out_0
silent) and the runtime `START_CAPTURE`/`STOP_CAPTURE` handlers (now registered in abox —
two formerly-dropped commands live) both apply. One-block linear ramp on transitions; when
closed, out_0 carries zeros (consumers stay connected, hear nothing) — capture gating is
now enforced in the data plane, not by consumer convention. Default open for bring-up;
covered by `test_capgate`.

**TTSOUT — playback gain / duck point (element, node pending).** Where the reply/narration
stream will be gained into the output once the `in_tts` port exists: per-block gain ramp =
the barge-in duck (mode `BARGE_IN_MUTING` drives it to 0), and the same tap feeds REF. ⛔
Today the column drives nothing, since playback bypasses abox entirely.

**vDMA-OUT — egress (structural).** Copies the slot's `chan[0]` into the `out_0` port buffer
with the atomic master gain applied (`SET_VOLUME`), silences unproduced ports, releases the
slot. The last Hermes instruction capture samples execute before they belong to PipeWire.

*(Deliberately **not** nodes: VAD — a separate always-on process per the perception design
(§16.7 known-gaps; silero in the ear process), with only a dumb onset-duck reflex ever
planned in-graph; and the wake-word spotter — VTS's own raw tap, never inside the cascade.)*

### 13.3 Mode gating & routing matrix (`abox_routing.c`, `abox_graph.c`)
`abox_mode` (`abox_node.h:21`):

| Mode | Value | Meaning |
|------|:---:|---------|
| `ABOX_MODE_KEYWORD_LISTENING` | 0 | idle/wake — all elements bypassed (AEC off) |
| `ABOX_MODE_BARGE_IN_MUTING` | 1 | user interrupted — full chain, **TTS out ducked → 0** |
| `ABOX_MODE_CONVERSATION` | 2 | full duplex |
| `ABOX_MODE_SYSTEM_RESET` | 3 | safe/muted |

Controllable elements (matrix columns, `abox_node.h:29`): `SRC, AEC, REF, BEAM, SES *(retired,
§13.2 — column reserved, always 0)*, CAPGATE, TTSOUT` (`ABOX_ELEM_COUNT=7`). A per-mode gain
matrix `kGain[4][7]` (`abox_routing.c:8`) → a
`uint32_t` active **bitmask** (`abox_active_mask`); the graph runs a stage only if its element bit
is set, else zero-copy passthrough. Mode is set by the control thread (`SET_MODE`) and read per
block with an **acquire load** — no lock.

### 13.4 Core-Proportional Buffer Pool & worker pool (`buffer_pipeline.*`, `worker_pool.*`)
- `HERMES_NUM_WORKER_CORES = 2` per-core isolated slots; each slot has its own `abox_frame` +
  backing `slot_mem`. `next_core_idx` rotates; `core_in_progress[]` are **atomic busy flags**.
- **Firewall:** if the target slot is still busy when a new period arrives → soft-drop the period
  (increment `drops`), driver ping-pong continues → **no ALSA Xrun**.
- Standing **worker pool** (no mutex, no malloc on hot path): workers self-claim jobs via atomic
  `fetch_add` on a cursor, spin with backoff then park on a generation counter. Pinned to A76,
  SCHED_FIFO 89. `abox_pool_run(..., deadline_ns, now_ns)` checks a deadline each spin; on overrun
  sets `force_abort` → caller emits **Soft-Mute**.

### 13.5 Soft-Mute & ducking
- **Soft-Mute** (`abox_node.h:102`): zero-fill the frame on a missed deadline (keeps AEC reference
  ring intact, cleaner than dropping input). Emits `AUDIO_CORE::evt::SOFT_MUTE`.
- **Ducking**: AEC blend factor derives from `abox_route_gain(mode, ELEM_AEC)`, ramped over ~10 ms;
  in `BARGE_IN_MUTING`, `TTSOUT` gain = 0 (narration silenced) while capture/AEC stay live.

### 13.6 Reference manager (`reference_manager.*`)
Provides AEC a **time-aligned post-fader far-end** (what the speaker emitted). Circular ring
`ABOX_REF_BULK_DELAY_MAX = 1536` samples (~96 ms @ 16 kHz); integer bulk delay `D_bulk` +
fractional residual with linear sub-sample interpolation. Tap = post-fader mixer or smart-amp
VI-Sense feedback (preferred). Seeded via `abox_ref_set_bulk_delay()`.

### 13.7 Lock-free parameter store (`param_store.h`)
Double-buffered, two fixed 256-B slots. RT side acquire-loads the active index; control side
release-stores a new slot then swaps. No torn reads, no mutex.

### 13.8 Offline harness (`harness_offline.c`)
Deterministic synthetic frames, no PipeWire daemon, same node vtables + routing matrix; exercises
all active modes and reports RMS per mode. Built as `hermes_offline_c`.

---

## 14. State Machines

### 14.1 Session FSM (`supervisor/SessionFsm.hpp/.cpp`) — ✅ implemented
The **conversational-turn** FSM is the authoritative orchestrator. States:

| State | Value | Meaning |
|-------|:---:|---------|
| `SS_INIT` | 0 | initialization |
| `SS_IDLE` | 1 | keyword listening (VTS armed), mode KEYWORD_LISTENING |
| `SS_CAPTURE` | 2 | streaming utterance to llm_connector |
| `SS_THINK` | 3 | awaiting first TTS chunk |
| `SS_SPEAK` | 4 | TTS playback, barge-in armed |
| `SS_BARGE_DUCK` | 5 | ducking → restart capture (barge-in) |
| `SS_FAULT` | 6 | reset + recover |
| `SS_SHUTDOWN` | 7 | terminal |

Transition table:

| Trigger (event) | From | Action | To |
|-----------------|------|--------|----|
| `WAKE_CONFIRMED` | IDLE | `startTurn()`: `DISARM` VTS, `SET_MODE{CONVERSATION}`, `OPEN_STREAM`, `START_CAPTURE` | CAPTURE |
| `STT_ENDPOINT` | CAPTURE | `UTTERANCE_END`, `STOP_CAPTURE` | THINK |
| `TTS_CHUNK` (first) | THINK | `PLAY_TTS`, `ARM_BARGE_IN` (reset `ttsEnded_`) | SPEAK |
| `TTS_STREAM_END` | any | set `ttsEnded_=true` (wait for drain) | *(hold)* |
| `PLAYBACK_DRAINED` ∧ `ttsEnded_` | SPEAK | `STOP_TTS`, `DISARM_BARGE_IN`, `SET_MODE{KEYWORD_LISTENING}`, `ARM` VTS | IDLE |

*(2026-07-19: `SET_MODE` now **carries the EngineMode as its body** — it was previously sent
bodyless, which `AudioCore::onSetMode` ignores (`abox_main.cpp:34`), leaving the mode machine
inert; `startTurn()` also gained the `SET_MODE{CONVERSATION}` it was missing.)*
| `BARGE_IN` (URGENT) | SPEAK | `ABORT` llm_connector (URGENT) only — **no SET_MODE here; duck is ABOX-local** | BARGE_DUCK |
| `MODE_CHANGED` | BARGE_DUCK | `STOP_TTS` + `startTurn()` (DISARM VTS, OPEN_STREAM, START_CAPTURE) | CAPTURE |
| `UNPLUGGED` (codec fault) | any | reset pipeline, abort llm, reset codec | FAULT |

`enter(s)` broadcasts `_Supervisor::evt::STATE_CHANGED`; entering `SS_FAULT` fires
`RESET_PIPELINE` (abox), `ABORT` (llm), `RESET` (codec). **Two-signal completion:** SPEAK→IDLE
requires **both** `TTS_STREAM_END` (LLM done) **and** `PLAYBACK_DRAINED` (audio drained).

**Not yet handled (TODO in the ctor, SessionFsm.cpp:42):** `START_SESSION` (PTT), `CANCEL_SESSION`,
`SHUTDOWN`, the FSM timeout events (`TO_*`), and `STT_NO_SPEECH → SS_IDLE`. **Barge-in design:** on
`BARGE_IN` the supervisor issues *only* the LLM `ABORT` and transitions; the time-critical duck is
performed **locally in ABOX** (which latches `BARGE_IN_MUTING` and emits `MODE_CHANGED` back). No
component currently produces `AUDIO_CORE::evt::BARGE_IN` (VAD node ⛔), so this path is dormant; the
dev GUI exercises ducking manually via `DUCK_PLAYBACK` + `SET_MODE`.

Threading: intake thread (`RecvMsgTask`, SCHED_FIFO 75) drains mq → `EventQueue`; FSM thread
(`FsmLoop`, SCHED_FIFO 70) is the **single consumer**; a worker pool (`start(2)`) handles non-RT
side tasks.

### 14.2 story_agent internal FSM (`story_agent/main.cpp`) — ⚠ basic
`enum class State { Idle, Reading, Paused, Interrupted }`. Drives narration independently of the
session FSM: `START`→Reading/`play(0)`; `PAUSE`→Paused; `RESUME`→Reading/`play(pos_)`;
`TTS_STREAM_END`→`play(++pos_)`; `STT_FINAL`→Interrupted→recall→resume `play(pos_)`;
`pos_ ≥ N`→`STORY_DONE`/Idle. *(The two FSMs coordinate via events, not a shared owner — see Risk
#6.)*

### 14.3 ABOX engine mode — see §13.3 (`abox_mode`, read per RT block).

---

## 15. High-Level Call Sequences (per use case)

**UC-1 Narration:** `supervisor → _Story::cmd::START → story_agent.play(0) → _Llm::cmd::PLAY_SEGMENT(idx)
→ llm_connector renders char voice → abox → speaker; TTS_STREAM_END → story_agent.play(++pos) …
until STORY_DONE`.

**UC-2 Wake-word turn:** `VTS → WAKE_CONFIRMED → supervisor.startTurn() (IDLE→CAPTURE) → STT →
STT_ENDPOINT (CAPTURE→THINK) → route()+LLM → TTS_CHUNK (THINK→SPEAK) → speaker → TTS_STREAM_END +
PLAYBACK_DRAINED (SPEAK→IDLE)`.

**UC-3 Barge-in turn:** `abox/VAD → BARGE_IN(URGENT) → supervisor.onBargeIn (SPEAK→BARGE_DUCK):
ABORT llm only (duck is ABOX-local — abox latches BARGE_IN_MUTING, ducks TTSOUT); MODE_CHANGED →
onModeChanged: STOP_TTS + startTurn() → CAPTURE. story_agent.onUserSpeech(STT_FINAL): Interrupted →
mem.recall → mem.remember → resume play(pos_)`. *(Dormant: no VAD producer of BARGE_IN yet.)*

**UC-4 Recall:** `story_agent.recall(user,q) → HTTP POST /search :7070 → mem0 → {facts} → inject into
answer prompt (TODO)`.

**UC-5 Consolidation:** ⛔ `idle/"good night" → read episodic → LLM distill → write OKF MemoryFacts →
refresh MEMORY.md → prune logs` (cloud-offloadable).

**UC-8 PTT:** *(planned)* `button/GUI → START_SESSION → supervisor.startTurn()` (skips VTS) — the
catalog id exists but the supervisor does not yet register a handler (SessionFsm.cpp:42 TODO).

**UC-9 Fault:** `codec → UNPLUGGED → enter(SS_FAULT) → RESET_PIPELINE/ABORT/RESET`.

**UC-11 Dev control:** `browser → gui_interface HTTP → CMsg (SET_MODE/SET_VOLUME/DUCK_PLAYBACK/
START_SESSION) → bus`.

---

## 16. Low-Level Detailed Sequences

§16.1–16.4 are message ladders for the core flows; §16.5–16.6 are **use-case walkthroughs for the
playback paths**, each with a data-plane block flow plus a control-plane call flow.

### 16.1 Wake-word interactive turn (UC-2)
```
VTS(3)            supervisor(1)/FSM            llm_connector(5)         abox(2)
  │ WAKE_CONFIRMED →  (IDLE)
  │  (evt 0x380)   │ startTurn():
  │                ├─ _VoiceTrigger::cmd::DISARM (0x301) → VTS
  │                ├─ _Llm::cmd::OPEN_STREAM (0x500) ─────► onOpen(): STT session +
  │                │                                       subscribe ABOX clean mono
  │                ├─ _AudioCore::cmd::START_CAPTURE (0x201) ───────────────► capture on
  │                └─ enter(SS_CAPTURE); STATE_CHANGED (0x180)
  │                                          STT runs … STT_ENDPOINT (0x584) ►│
  │                │◄────────────────────────────────────────────────────────
  │                │ onSttEndpoint(): _Llm::cmd::UTTERANCE_END (0x502) ─────► onUttEnd():
  │                │                   _AudioCore::cmd::STOP_CAPTURE (0x202)   route(text)
  │                │ enter(SS_THINK)                                          local|cloud
  │                │                          LLM_BEGIN (0x586) ◄────────────
  │                │                          TTS_CHUNK (0x587, first) ◄─────  TTS synth
  │                │ onTtsChunk(): play, arm barge-in; enter(SS_SPEAK)        PLAY_TTS→abox
  │                │                          TTS_STREAM_END (0x588) ◄────────
  │                │ ttsEnded_=true
  │                │                          PLAYBACK_DRAINED (0x286) ◄───────────────────
  │                │ onDrained(): STOP_TTS, DISARM_BARGE_IN,
  │                │   SET_MODE(KEYWORD_LISTENING) → abox, ARM VTS; enter(SS_IDLE)
```

### 16.2 Barge-in (UC-3) — the key path
```
abox(2)/VAD       supervisor(1)/FSM           llm_connector(5)     story_agent(8)
  │ (VAD fires) — ABOX LOCALLY latches BARGE_IN_MUTING, ducks TTSOUT→0  (data-plane, no cmd in)
  │ BARGE_IN ────► (SPEAK)
  │ (0x281,URGENT)│ onBargeIn():
  │               ├─ _Llm::cmd::ABORT (0x503, URGENT) ──► onAbort(): cancel inference+TTS
  │               └─ enter(SS_BARGE_DUCK)         (NO SET_MODE/DUCK issued — duck already local)
  │ MODE_CHANGED ►│ onModeChanged(): _AudioCore::cmd::STOP_TTS (0x204) + startTurn()
  │ (0x28B)       │   (DISARM VTS, OPEN_STREAM, START_CAPTURE); enter(SS_CAPTURE)
                  │                          STT_FINAL (0x583) ──────────────► onUserSpeech():
                  │                                                            state=Interrupted
                  │                                              recall("listener",q)→/search
                  │                                              remember("listener",q)→/add
                  │                                              state=Reading; play(pos_)
```
**Design & latency note:** the duck is **data-plane-local in ABOX** (the supervisor issues only the
LLM `ABORT`); ABOX emits `MODE_CHANGED` after the mode latches. The ≤12 ms KPI (NFR-3) and indeed
*any* barge-in today depend on a **data-plane VAD node** emitting `BARGE_IN` on the URGENT lane —
⛔ not built, so the path is dormant. The dev GUI simulates ducking via `DUCK_PLAYBACK` + `SET_MODE`.

### 16.3 Story segment advance (UC-1)
```
story_agent(8)                         llm_connector(5)            abox(2)
  onSegmentDone(TTS_STREAM_END 0x588):
   play(++pos_):
     StorySegmentRef{idx} →
     _Llm::cmd::PLAY_SEGMENT (0x504) ──► resolve idx→speaker/tone, render expressive TTS ──► out_0
     _Story::evt::SEGMENT_STARTED (0x880) → supervisor
   if pos_ ≥ N: _Story::evt::STORY_DONE (0x881); state=Idle
```
*(`PLAY_SEGMENT` handler in llm_connector is ⛔ not yet implemented.)*

### 16.4 Memory recall/remember (UC-4)
```
story_agent.Memory (in-proc facade)            services/memory/server.py (:7070)
  recall(user,q):  POST /search {user_id,query,top_k:3} ──► mem().search() ──► {"facts":[...]}
                   (HTTP, POSIX socket, 2 s timeout; "" on any failure → graceful degrade)
  remember(user,t):POST /add    {user_id,text}          ──► mem().add() (LLM extraction, heavy)
                   (fire-and-forget; TODO: dispatch off the IPC thread)
  health:          GET  /health                          ──► {"ok":true,"mem0":bool}
```
Backends (lazy-init, all-local capable): vector store Chroma `MEM0_DB=/var/lib/hermes/mem0`,
embeddings `huggingface/all-MiniLM-L6-v2`, extraction LLM via `ollama` (e.g. `qwen2.5:3b`) or
OpenAI. 503 + empty facts when a backend isn't ready (agent still runs).

### 16.5 Use Case — Basic Playback Path (cached audio, no synthesis)
**Goal:** narrate a story segment whose audio is **pre-rendered and cached** (UC-1). No TTS engine
runs on the device — lowest latency, fully offline. This is the audio-transport spine that the
loopback test (SYS-04) exercises end-to-end.

**Block flow (data plane — audio bytes, NOT on the control bus):**
```
 segments_[idx] ──► llm_connector resolves idx → cached PCM clip (file / SHM cache)
                          │  PCM
                          ▼
                    PipeWire playback stream ─────────────────────────────► speaker sink
                          │ post-fader tap (far-end reference)
                          ▼
                    ABOX reference_manager  ◄── AEC subtracts the played audio from the mic
 ── parallel capture spine (always live) ──────────────────────────────────────────────────
 mic ─in_0,in_1─► hermes.abox:  SRC → AEC → BEAM → DMX → CAPGATE ─out_0─► clean mono (→ STT / VAD)
                  (mode-gated cascade, 240-frame / 5 ms quantum, A76 worker pool)
```

**Call flow (control plane — CMsg ladder):**
```
supervisor(1)        story_agent(8)              llm_connector(5)            abox(2)/PipeWire
  _Story::cmd::START ─►
  (0x800)              onStart: pos_=0; play(0):
                        _Llm::cmd::PLAY_SEGMENT{idx=0} ─► resolve idx→cached clip
                        (0x504, body StorySegmentRef)     open PW playback stream
                        _Story::evt::SEGMENT_STARTED ─►   write PCM ───────────► speaker
                        (0x880) → supervisor              (tap → AEC reference)
                                                          …clip drains…
                                                          _Llm::evt::TTS_STREAM_END ─►│
                        onSegmentDone ◄───────────────────(0x588)                     │
                        play(++pos_)  ── loop ──
                        …at pos_ ≥ N:_Story::evt::STORY_DONE (0x881) ─► supervisor; state=Idle
```
**Status:** ⚠ framework — `story_agent` loop, `PLAY_SEGMENT`/`SEGMENT_STARTED`/`STORY_DONE` and the
position pointer are built; the **`PLAY_SEGMENT` handler in `llm_connector` is ⛔ not implemented**
(no clip resolution / PW playback yet). On the capture spine SRC is real; AEC/BEAM are
passthrough stubs and DMX is unbuilt (ABOX-11 asserts bit-exact transport today).

### 16.6 Use Case — Playback with TTS (on-the-fly synthesis)
**Goal:** speak audio that is **not cached** — an interactive answer (UC-2/UC-3) or an uncached
segment — by synthesizing expressive TTS (per-character voice + emotion) on demand, local or cloud,
through the guardrail gate, then into the same playback path. Higher latency than §16.5; the audio
source is a live TTS stream instead of a cached clip.

**Block flow (data plane):**
```
 utterance / segment text (speaker,tone)  [resolved locally; never on the bus]
        │
        ▼  llm_connector
   route(text):  ≤120 chars → LOCAL on-device LLM+TTS (llama.cpp / NPU)  ──┐
                 > 120 chars → CLOUD LLM/TTS over socket  ─────────────────┤  PCM stream
                                                                           ▼
        guardrail OUTPUT GATE (before any audio) ──► PipeWire playback ──► speaker sink
                                                          │ post-fader tap
                                                          ▼
                                                 ABOX reference_manager (AEC far-end)
```

**Call flow — A) interactive answer (drives the Session FSM, SS_THINK→SS_SPEAK):**
```
abox(2)            llm_connector(5)                       supervisor(1)/FSM        speaker
  STT_ENDPOINT ──► (via supervisor.onSttEndpoint)
  (0x584)          ◄─ _Llm::cmd::UTTERANCE_END (0x502) ── onSttEndpoint (CAPTURE→THINK)
                   route(transcript): local|cloud
                   _Llm::evt::LLM_BEGIN (0x586) ─────────► (notify)
                   synth TTS [guardrail gate] → _Llm::evt::TTS_CHUNK (0x587) ─► onTtsChunk:
                                                                  _AudioCore::cmd::PLAY_TTS (0x203) ─► ▶
                                                                  ARM_BARGE_IN; (THINK→SPEAK)
                   _Llm::evt::TTS_STREAM_END (0x588) ────► onTtsStreamEnd: ttsEnded_=true
  PLAYBACK_DRAINED (0x286) ─────────────────────────────► onPlaybackDrained (∧ttsEnded_):
                                                                  STOP_TTS, SET_MODE(KWD), ARM VTS;
                                                                  (SPEAK→IDLE)
```

**Call flow — B) story segment synthesized (story_agent loop, no FSM turn):**
```
story_agent(8)        llm_connector(5)                                   abox(2)/PipeWire
  PLAY_SEGMENT{idx} ─► resolve idx → (speaker,tone,text)
  (0x504)              route(text); synth expressive TTS (voice+emotion)
                       [guardrail gate] → stream PCM ──────────────────► speaker (tap→AEC ref)
                       _Llm::evt::TTS_STREAM_END (0x588) ─► onSegmentDone → play(++pos_)
```
**Status:** ⚠ skeleton — `route()` (120-char heuristic) and the `UTTERANCE_END→LLM_BEGIN`/
`TTS_CHUNK→PLAY_TTS` wiring through the FSM exist; **`runLocal`/`runCloud` (actual STT/LLM/TTS) and
the guardrail output gate are ⛔ stubs/TODO**, and the `PLAY_SEGMENT` synthesis branch (B) is unbuilt.
*(Partially superseded — see §16.7 for the as-built voice loop: STT is resident-in-process and the
cloud LLM call is real as of the v0.3 capture slice.)*

### 16.7 End-to-End Data Flow — mic → text → cloud → speaker (as-built, 2026-07-20)

The complete life of one interactive turn's data, hop by hop, exactly as the code stands after
the **v0.3 capture slice** (out_0 → `AudioRing` → resident STT). Legend: ✅ built · 🆕 built in
this slice · ⚠ partial · ⛔ pending.

```
════ DATA PLANE — PipeWire graph, one 5 ms cycle, zero-copy SHM ═══════════════════════════════
 I2S DMA ─IRQ─► ALSA source node (the graph DRIVER; the mic crystal IS the system clock)
                    │ fan-out: every consumer gets the same period, same cycle
        ┌───────────┼───────────────────────┐
        ▼           ▼                       ▼
  hermes.abox    hermes-vts-mic         hermes-cc-mic
  in_0,in_1      (VTS raw tap 16 k)     (llm_connector; HERMES_PW_CAP_TARGET=hermes.abox
  48 k f32       ├► PrerollRing (SHM,    links it to out_0 🆕 — unset ⇒ raw mic)
        │        │   3 s history) ✅          ▲
  vDMA-IN ✅     └► KWS "hey aria" ✅         │ same-cycle SHM link 🆕
  SRC ✅→AEC ⚠→BEAM ⚠→DMX ✅→CAPGATE ✅ ── vDMA-OUT ══ out_0 (mono·f32·48 k, §13.2 contract)
════ CONSUMER EDGE — llm_connector process ════════════════════════════════════════════════════
  s_capture (PW RT thread): ring_.push(240 fr) 🆕      [lock-free SPSC AudioRing<96000>, 2 s]
  sttLoop (worker): pop 100 ms chunks → resident sherpa streaming zipformer 🆕
      AcceptWaveform(48 k) → internal 16 k features → incremental decode (partials live)
  vadLoop: RMS endpoint (400 ms speech / 900 ms silence) ✅   [target: ear-VAD SPEECH_OFF ⛔]
      endpoint → finalize handshake (finalizeGen_/turnGen_) 🆕 → ★ TRANSCRIPT (UTF-8 string) ★
                                        ── AUDIO DIES HERE, ON-DEVICE ──
════ CONTROL PLANE — POSIX-mq CMsg (20 B hdr + ≤256 B) ════════════════════════════════════════
  WAKE_CONFIRMED 0x380 {wake_pos,from_pos,epoch} ✅ (body unread ⛔) → FSM startTurn:
  DISARM 0x301 · SET_MODE{CONVERSATION} 🆕 · OPEN_STREAM{wake body → preroll} 🆕 · START_CAPTURE 🆕(capgate)
  STT_ENDPOINT 0x584 → UTTERANCE_END 0x502 → … → STT_FINAL 0x583 {text} (⛔ unrouted — dropped)
════ NETWORK — the ONLY hop that leaves the device, string-only ═══════════════════════════════
  groq_chat(): transcript in JSON ──HTTPS──► api.groq.com llama-3.1-8b ──SSE──► reply text ✅
════ RETURN PATH — text back to air ═══════════════════════════════════════════════════════════
  run_tts(): Piper subprocess ⚠ → 22.05 k PCM → ttsWav_ (gen-gated install 🆕)
  s_playback: connector's OWN PW stream → speaker  ⚠ [target: abox in_tts → TTSOUT duck ⛔]
  TTS_CHUNK 0x587 / TTS_STREAM_END 0x588 → FSM; PLAYBACK_DRAINED 0x286 emitted by the
  connector (impersonating module 2 ⚠ — truthful only once playback transits abox)
```

**Hop table** (payload · unit · mechanism · code · status):

| # | Hop | Payload / format | Unit / rate | Mechanism | Code | |
|---|-----|------------------|-------------|-----------|------|:---:|
| 1 | I2S→ALSA→driver | s16/s32 PCM 2 ch | DMA period / 5 ms | hardware + kernel | — | ✅ |
| 2 | driver → consumers | f32 PCM | 240 fr / cycle | PW fan-out, SHM zero-copy, eventfd | `Pw.cpp` | ✅ |
| 3 | abox cascade | f32 2 ch → 1 ch clean | in-place per block | vDMA-IN → mask-gated nodes → vDMA-OUT | `buffer_pipeline.c` | ⚠ kernels |
| 4 | out_0 → connector | f32 mono 48 k | 240 fr, same cycle | PW link (`node.target`, WirePlumber) | `Pw.cpp:187` | 🆕 |
| 5 | RT → worker | f32 samples | push 5 ms / pop 100 ms | `AudioRing<96000>` lock-free SPSC | `AudioRing.hpp` | 🆕 |
| 6 | audio → **string** | PCM → UTF-8 transcript | streaming, final ~ms after endpoint | resident sherpa zipformer (loads once at `Start()`) | `llm_connector` `sttLoop` | 🆕 |
| 7 | wake pointer | `{wake_pos,from_pos,epoch}` 20 B | per wake | CMsg on mq — a *pointer into the past*, never samples | `PrerollRing.hpp` | ✅ sent / ⛔ read |
| 8 | turn control | CMsg ids (§12) | 20–276 B | POSIX mq, 3 prio lanes | `MsgBus.cpp` | ✅ |
| 9 | **string → cloud** | transcript in JSON | one HTTPS request | connector's TLS socket (libcurl) — *the only network hop* | `groq_chat()` | ✅ |
| 10 | reply → PCM | text → f32 22.05 k | per reply | Piper subprocess | `run_tts()` | ⚠ |
| 11 | PCM → speaker | f32 | 5 ms cycles | connector's own PW playback stream (⛔ not via abox `in_tts` yet) | `s_playback` | ⚠ |

#### 16.7.1 Detailed walkthrough — mic → STT → cloud (call-level, one utterance)

Thread legend: **[PW-abox]** abox's PipeWire RT thread · **[PW-cc]** the connector's PipeWire RT
thread · **[stt]** connector `sttLoop` worker · **[vad]** connector `vadLoop` · **[recv]**
connector MsgBus recv thread · **[pipe]** the per-turn detached `pipeline()` thread ·
**[fsm]** supervisor FSM thread.

```
① CAPTURE — every 5 ms, forever
   [PW-abox] driver tick → on_process (Pw.cpp:58)
       n = pos->clock.duration                          = 240 frames
       in0/in1 = pw_filter_get_dsp_buffer(port, 240)    2 × 960 B, graph SHM
       abox_block → hermes_pipeline_process_tick:
         core_in_progress[slot]? busy → soft-drop (drops++), else:
         bp_ingest: memcpy in0,in1 → slot.frame          2 × 960 B
         graph tick (mask-gated): SRC(real)→AEC(stub)→BEAM(stub: ch0)
         bp_egress: memcpy frame.ch0 × gain → out_0      960 B
   cost ≈ tens of µs; deadline 5 ms; overrun ⇒ Soft-Mute zero-fill, never an Xrun

② DELIVERY — same cycle
   link hermes.abox:out_0 → hermes-cc-mic (created once by WirePlumber from
   node.target="hermes.abox"): the graph maps the SAME memfd pages into the
   connector and rings its eventfd → [PW-cc] wakes, dequeues the buffer, calls
     s_capture(user, samples, 240, rate):
       rms_ ← √(Σs²/240)                  (feeds the VAD; atomic store)
       capturing_? ring_.push(samples,240) — wp_+=240 (release); full ⇒ drop-new,
                                            overruns_+=n (never blocks, never allocs)
   RT work per cycle: one memcpy-equivalent + 2 atomics.  ring high-water ≈ 2 cycles

③ RECOGNITION — concurrent with capture (this is why the transcript is "instant")
   [stt] loop: n = ring_.pop(chunk, 4800)               ≤100 ms per pop
       gate: !capturing_ && finalizeGen_==0 ⇒ discard (stale idle block)
       SherpaOnnxOnlineStreamAcceptWaveform(stream, 48000, chunk, n)
         └ internal: 48 k→16 k resample → 80-dim fbank (25 ms win / 10 ms hop)
       while IsOnlineStreamReady: DecodeOnlineStream    zipformer chunk-16
         └ transducer greedy search → hypothesis grows: "why" → "why is the sky…"
   model resident since Start() (one ~4 s load at boot — never per-utterance)

④ ENDPOINT — deciding the child stopped
   [vad] every 50 ms: rms_ > 0.008 ? speechMs+=50 : silenceMs+=50
       speechMs ≥ 400 ⇒ hadSpeech; hadSpeech ∧ silenceMs ≥ 900 ⇒
       SendMsg(SUPERVISOR, STT_ENDPOINT 0x584)          20 B CMsg, mq
   [fsm] onSttEndpoint (SS_CAPTURE): UTTERANCE_END 0x502 → connector,
       STOP_CAPTURE 0x202 → abox (capgate closes 🆕), enter(SS_THINK)
   [recv] onUttEnd: capturing_=false; finalizeGen_=turnGen_; spawn [pipe]

⑤ FINALIZE — the string is born
   [stt] ring drains to empty → sees finalizeGen_≠0 (exchange→g):
       InputFinished → decode remaining → GetResult → text
       StreamReset (fresh for next turn)
       lock(pcmMtx_): turnGen_==g ? { sttResult_=text; sttDone_=true } : discard(stale)
       sttCv_.notify_all()
   [pipe] wakes from sttCv_.wait_for (predicate: sttDone_‖abort_‖stopping_‖gen-change)
       → transcript in hand, ~ms after the endpoint fired

⑥ CLOUD — the only bytes that leave the device
   [pipe] SendMsg(SUPERVISOR, STT_FINAL 0x583, text)    ≤256 B (⛔ unrouted today)
       hist ← copy(history_) under lock
       groq_chat(key, transcript, hist):
         POST https://api.groq.com/openai/v1/chat/completions   TLS, timeout 30 s
         { "model":"llama-3.1-8b-instant",
           "messages":[ {system: kSystemPrompt}, …hist…, {user: transcript} ] }
         ← choices[0].message.content → reply
       lock: turnGen_ still mine? append (user,assistant) to history_ (cap 20)
   key from GROQ_API_KEY env / /etc/anime-ai/secrets.env — provisioned, not embedded
```

Timing profile of the whole chain (real A76 target): speech is decoded **while spoken** (③ runs
concurrently with ①/②), so end-of-speech → transcript ≈ the 900 ms VAD silence window + ~ms of
finalize; transcript → first reply token is the cloud round-trip. The 4 s model load that used
to sit inside every turn is now paid once at boot. On QEMU (≈4.5× RTF) ③ lags live audio and
the finalize wait absorbs the backlog (bounded at 60 s).

**The three transformation boundaries** (each is a one-way door):
1. **dirty → clean** (hop 3): 2-mic raw becomes one echo-cancellable mono — everything that
   *listens* consumes this, nothing upstream of it.
2. **audio → string** (hop 6): the transcript is born **on-device**; no audio survives past the
   recognizer. This is the privacy boundary (§9.3/NFR-9) expressed as data flow.
3. **string → audio** (hop 10): the reply is re-embodied locally; the network never carries a
   sample in either direction.

**Standing invariants** (violations are architecture bugs): the mq bus never carries audio
(≤276 B messages make it physically impossible — 5 ms of audio is 960 B); PipeWire never touches
the network; the network carries only strings; bulk audio moves exclusively by shared memory
(PW buffers, PrerollRing, `/hermes.out0` when the TAP node lands).

**Where audio is ever at rest** (complete buffering inventory): PrerollRing (3 s sliding, SHM) ·
`AudioRing` (≤2 s transit, normally ~10 ms) · sherpa decoder state (features, not PCM) ·
`ttsWav_` (one reply, until drained). Nothing else holds samples; there is no disk in the live
path (the `/tmp` WAV exists only in the no-sherpa fallback).

**Known gaps this section makes visible** (tracked in §21/§22 — updated 2026-07-20 after the
Tier-1 batch): ~~`STT_FINAL` unrouted~~ → now delivered to `story_agent` ✅; ~~CAPGATE
unimplemented~~ → structural gate node + `START/STOP_CAPTURE` handlers ✅; ~~preroll backfill
unread~~ → `WakeConfirmedBody` forwarded through `OPEN_STREAM`, ring history spliced into the
recognizer before live audio ✅. Still open: playback bypasses abox (`PLAYBACK_DRAINED`
impersonated, TTSOUT duck inert — `in_tts` port pending); VAD endpoint is the connector's RMS
loop, not the ear-process silero design; WirePlumber link rule is launch-script/env, not yet
target-image policy.

### 16.8 UC-12 — Audio Capture (mic → STT → cloud → spoken answer)

**Goal:** the listener speaks after the wake word; the device turns their speech into text
**on-device**, sends only that text to the cloud LLM, and speaks the reply — the canonical
interactive turn as the audio subsystem sees it. (UC-2 describes the same turn from the FSM's
perspective; UC-12 is its data-path realization, detailed hop-by-hop in §16.7/§16.7.1.)

**Actors:** listener · VTS (wake + preroll) · abox (clean feed) · llm_connector (ring →
resident STT → cloud → TTS) · supervisor FSM · cloud LLM (Groq today).
**Preconditions:** abox running with mic links; connector launched with
`HERMES_PW_CAP_TARGET=hermes.abox`; STT model present (else subprocess fallback); network up
(else `LLM_ERROR` → IDLE).
**Trigger:** `WAKE_CONFIRMED` (today's only working key; PTT and VAD barge-in are the two
designed-but-unwired alternates, §5 UC-8/UC-3).

**Main flow** (compressed — full call-level ladder in §16.7.1):
```
1  wake → FSM startTurn: DISARM VTS · SET_MODE{CONVERSATION} · OPEN_STREAM · enter CAPTURE
2  every 5 ms: mic ═ abox SRC→AEC→BEAM→DMX→CAPGATE ═ out_0 ═(PW link)═ s_capture → AudioRing.push
3  concurrently: sttLoop pops 100 ms chunks → resident sherpa decodes WHILE the child speaks
4  RMS-VAD endpoint (400/900 ms) → STT_ENDPOINT → FSM: UTTERANCE_END · enter THINK
5  sttLoop drains ring → finalize → transcript published (gen-gated) → pipeline() wakes
6  transcript → STT_FINAL on the bus · transcript JSON → HTTPS → cloud LLM → reply text
7  reply → Piper TTS → connector playback stream → speaker; TTS_CHUNK/…/PLAYBACK_DRAINED
8  FSM: STOP_TTS · SET_MODE{KEYWORD_LISTENING} · ARM VTS · enter IDLE
```

**Call sequence (as-built v0.3)** — one continuous graph, transducer to cloud and back.
Lanes: the PipeWire graph/driver (hardware side) · abox(2) · VTS(3) · supervisor(1) · 
llm_connector(5) · cloud. `═` data plane (SHM, per-cycle) · `─` control plane (CMsg/mq) · 
numbered banners are phases of the SAME timeline, not separate diagrams.

```
mic/ALSA/PW graph    abox(2)             VTS(3)              supervisor(1)/FSM       llm_connector(5)      CLOUD
(the driver)            │                   │                        │                      │
── ① CONTINUOUS INGRESS — every 5 ms, forever (hardware → driver → callbacks) ────────────────────────────────
ADC ─I2S/TDM─► DMA      │                   │                        │                      │
 period-complete IRQ    │                   │                        │                      │
 (CPU0-3, irqaffinity)  │                   │                        │                      │
 ALSA source = DRIVER,  │                   │                        │                      │
 cycle N: s16→f32 into  │                   │                        │                      │
 SHM; fan-out + eventfd:│                   │                        │                      │
 ╠═ in_0,in_1 (48 k) ══►│ on_process (Pw.cpp:58, RT thread):         │                      │
 ║                      │  n = pos->clock.duration (=240)            │                      │
 ║                      │  pw_filter_get_dsp_buffer(port, n)         │                      │
 ║                      │  └ abox_block → hermes_pipeline_process_tick():                   │
 ║                      │     bp_ingest  = vDMA-IN.process  (2×960 B → slot)                │
 ║                      │     abox_graph_tick(mask 0x00): all SKIP…  │                      │
 ║                      │       …except dmx_process (STRUCTURAL — runs every mode)          │
 ║                      │     bp_egress  = vDMA-OUT.process ×gain ═ out_0 (idle, unread)    │
 ╚═ vts-mic (→16 k) ═══════════════════════►│ s_mic (PW RT thread):  │                      │
                        │                   │  Preroll_Write (3 s SHM ring, writePos++)     │
                        │                   │  audioRing_.push → kwdLoop: pop 1600 →        │
                        │                   │  AcceptWaveform → Decode … "hey aria" HIT     │
── ② WAKE → TURN START ───────────────────────────────────────────────────────────────────────────────────────
                        │                   ├─ WAKE_CONFIRMED 0x380 ─►│ onWake [SS_IDLE]:   │
                        │                   │   {wake,from,epoch}     │ startTurn()         │
                        │                   │◄────── DISARM 0x301 ────┤                     │
                        │◄─────────── SET_MODE 0x200 {2=CONV} 🆕 ─────┤ (latch mask 0x6F)   │
                        │                   │                         ├─ OPEN_STREAM 0x500 ─►│ onOpen: ++turnGen_,
                        │◄─────────── START_CAPTURE 0x201 🆕 ────────┤                      │ capturing_=true
                        │  capgate opens (ramped)                    │                      │
                        │                   │                         └ enter(SS_CAPTURE)   │
── ③ CAPTURE + LIVE STT — every cycle N (data plane; never on the bus) ───────────────────────────────────────
 ╠═ in_0,in_1 ═════════►│ on_process → …process_tick():              │                      │
                        │  firewall: slot busy? soft-drop (drops++)  │                      │
                        │  vDMA-IN.process: ports → slot.frame       │                      │
                        │  abox_graph_tick(mask 0x6F):               │                      │
                        │    src_process   (ratio 1.0 fast-path ✅)  │                      │
                        │    aec_process   (ref read — zeros ⚠, bypass)                     │
                        │    beam: default_process (2→2 bypass ⚠)    │                      │
                        │    dmx_process:  channels=1 (select ch0 🆕)│                      │
                        │    (CAPGATE·TTSOUT: matrix columns only —  │                      │
                        │     no node files yet ⛔; REF is read      │                      │
                        │     inside aec_process, not a node)        │                      │
                        │  vDMA-OUT.process: chan[0] × gain          │                      │
                        │  [async pool: worker@cpu5/6 runs the tick; │                      │
                        │   deadline miss → abox_soft_mute zero-fill]│                      │
                        ╞═ out_0 (mono·f32·48 k) ═(link, same cycle)═══════════════════════►│ stream evt →
                        │                   │                        │                      │ s_capture(): rms_;
                        │                   │                        │                      │  ring_.push(240) 🆕
                        │                   │                        │                      │ sttLoop: ring_.pop(4800)
                        │                   │                        │                      │  AcceptWaveform(48 k) →
                        │                   │                        │                      │  IsReady/Decode… live 🆕
── ④ ENDPOINT → TRANSCRIPT (audio dies on-device) ────────────────────────────────────────────────────────────
                        │                   │                        │                      │ vadLoop (50 ms tick):
                        │                   │                        │                      │  rms_>0.008: speechMs+=50
                        │                   │                        │                      │  ≥400 ⇒ hadSpeech; then
                        │                   │                        │                      │  silenceMs ≥900 ⇒
                        │                   │                        │◄─ STT_ENDPOINT 0x584 ┤  SendMsg(0x584)
                        │                   │        onSttEndpoint:  │                      │
                        │◄─────────── STOP_CAPTURE 0x202 🆕 ─────────┤                      │
                        │  capgate closes → out_0 silent             │                      │
                        │                   │                        ├─ UTTERANCE_END 0x502 ►│ onUttEnd:
                        │                   │                        └ enter(SS_THINK)      │  capturing_=false
                        │                   │                        │                      │  finalizeGen_=turnGen_ 🆕
                        │                   │                        │                      │  spawn pipeline() thread
                        │                   │                        │                      │ sttLoop (ring now empty):
                        │                   │                        │                      │  finalizeGen_.exchange(0)
                        │                   │                        │                      │  InputFinished(stream)
                        │                   │                        │                      │  while IsReady: Decode
                        │                   │                        │                      │  GetOnlineStreamResult
                        │                   │                        │                      │   → r->text; DestroyResult
                        │                   │                        │                      │  StreamReset (next turn)
                        │                   │                        │                      │  lock(pcmMtx_): gen==mine?
                        │                   │                        │                      │   sttResult_=text,
                        │                   │                        │                      │   sttDone_=true 🆕
                        │                   │                        │                      │  sttCv_.notify_all()
                        │                   │                        │                      │ pipeline(): wait_for
                        │                   │                        │                      │  (sttDone_‖abort_‖stop‖
                        │                   │                        │                      │   gen-change 🆕) wakes →
                        │◄─ STT_FINAL 0x583 {text} ⛔no handler ─────────────────────────── ┤ ★ "why is the sky blue"
── ⑤ CLOUD ROUND-TRIP — the only network hop; string only ────────────────────────────────────────────────────
                        │                   │                        │                      │ pipeline() continues:
                        │                   │                        │                      │  lock: hist=copy(history_) 🆕
                        │                   │                        │                      │  groq_chat(key,text,hist):
                        │                   │                        │                      │   json_escape + messages[]
                        │                   │                        │                      │   curl_easy_setopt(URL,
                        │                   │                        │                      │    Bearer, body, 30 s)
                        │                   │                        │                      ├─ curl_easy_perform ─►│
                        │                   │                        │                      │  {system,hist,user}  │ Groq
                        │                   │                        │                      │◄─ choices[0].message ┤ llama-3.1
                        │                   │                        │                      │   .content = reply   │
                        │                   │                        │                      │  lock: gen==mine? append
                        │                   │                        │                      │   (user,asst)→history_ 🆕
                        │                   │                        │                      │ run_tts(reply): popen
                        │                   │                        │                      │  piper → load_wav →
                        │                   │                        │                      │  lock+gen: install ttsWav_ 🆕
── ⑥ SPEAK → TURN END ────────────────────────────────────────────────────────────────────────────────────────
                        │                   │                        │◄─ TTS_CHUNK 0x587 ───┤ (+ TTS_STREAM_END
                        │                   │          onTtsChunk:   │                      │  0x588 back-to-back —
                        │◄──── PLAY_TTS 0x203 · ARM_BARGE_IN 0x208 ──┤ ⛔both dropped        │  two-signal degenerates)
                        │                   │                        ├ enter(SS_SPEAK)      │
                        │                   │                        │ onTtsStreamEnd:      │
                        │                   │                        │  ttsEnded_=true      │
 🔊 ◄═ speaker ◄════════ connector's OWN PW playback stream ◄══════ s_playback drains ══════╡ ⚠ bypasses abox
                        │                   │                        │◄─ PLAYBACK_DRAINED ──┤ (module 5 emits an
                        │                   │   onPlaybackDrained:   │   0x286              │  AUDIO_CORE id ⚠)
                        │◄──── STOP_TTS · DISARM_BARGE_IN ⛔dropped ─┤                      │
                        │◄─────────── SET_MODE 0x200 {0=KWD} 🆕 ─────┤ (latch mask 0x00)    │
                        │                   │◄────── ARM 0x300 ──────┤                      │
                        │                   │ armed_=true            └ enter(SS_IDLE)  ⟲ back to ①
```

**Alternate flows:** A1 *no speech* — VAD max-utterance cap or empty transcript →
`STT_NO_SPEECH` → IDLE. A2 *abort/barge* — `ABORT` sets `abort_` + wakes the finalize wait;
the turn's `pipeline()` exits at the next gate; a superseded turn is inert (generation
counter). A3 *no sherpa at build* — `sttLoop` accumulates PCM; `run_stt()` subprocess
transcribes at the endpoint (adds ~4 s model load per turn). A4 *abox absent* — targeted
stream stays silent; unset the env to fall back to the raw mic (dev mode; forfeits AEC).
A5 *test stub* — `HERMES_TEST_UTTERANCE` short-circuits step 5–6's STT with a fixed string.

**Postconditions:** transcript+reply appended to `history_` (cap 20); no audio persisted
anywhere (§16.7 buffering inventory); device re-armed in IDLE.

**Status:** steps 1–6 ✅ as of the v0.3 capture slice + Tier-1 batch (2026-07-20) —
clean-feed capture 🆕, `AudioRing` seam 🆕, resident streaming STT 🆕, mode body 🆕,
**CAPGATE gate + `START/STOP_CAPTURE` handlers 🆕, preroll backfill 🆕 (gapless from the
true utterance start), `STT_FINAL` → story_agent 🆕**; step 7 ⚠ (playback bypasses abox —
`in_tts`/TTSOUT pending, `PLAYBACK_DRAINED` impersonated); guardrail output gate ⛔.
Runtime validation on QEMU/target pending.

---

## 17. Knowledge & Memory Subsystem

### 17.1 Two layers — curated vs dynamic
**Curated** (static, human/agent-edited OKF md): `brain/guardrails.md`, persona, `characters/*.md`,
book lore — loaded by **role** (always-on) or **local search**. **Dynamic** (from conversation):
listener facts + Q&A history — written by **consolidation** into `brain/memory/semantic/*.md`.

```
CURATED (static, human/agent-edited, OKF md)          DYNAMIC (evolving, from conversation)
  guardrails · persona · characters · book lore         listener facts, story Q&A history
  ──────────────────────────────────────────           ──────────────────────────────────────
  read by ROLE (always-load) or LOCAL SEARCH            written by CONSOLIDATION → OKF
```

### 17.2 OKF knowledge model
Each knowledge unit = markdown + YAML frontmatter (`type` required; `title/description/tags/
timestamp/resource` reserved), linked by md links; `index.md` for discovery, `log.md`/`MEMORY.md`
for history. Types used: `Guardrails, Persona, Character, BookFact, ListenerProfile, MemoryFact`.
Portable, git-friendly, parent-viewable via OKF's offline HTML visualizer (no backend, no data
leaves the page).

### 17.3 Memory facade (3-call seam, `story_agent/main.cpp`)
```cpp
std::string recall (const std::string& user, const std::string& query); // top-k facts (pre-LLM)
void        remember(const std::string& user, const std::string& turn);  // append episodic
void        exportMd (const std::string& user);                          // parent snapshot (⛔ stub)
```
Backend selected by `HERMES_MEM_BACKEND`: `local` (OKF search ⛔) | `mem0` (sidecar ✅) | `cloud`
(⛔). Endpoint `HERMES_MEM_HOST/PORT` (default `127.0.0.1:7070`).

### 17.4 Memory lifecycle — tiered, consolidated at idle
```
working (last N turns in prompt)
episodic (memory/sessions/<ts>.md, raw, append per turn)
   ── idle "sleep" job (⛔) ──► semantic OKF MemoryFacts (ADD/UPDATE/DELETE) + MEMORY.md refresh
                                                          → prune episodic logs
```
**Writes happen off the turn** (per-turn = cheap append; LLM extraction at idle, cloud-offloadable)
— the footprint-safe alternative to per-turn extraction.

### 17.5 Retrieval tiers — local search over OKF (mem0 optional)
| Tier | Mechanism | Use |
|------|-----------|-----|
| Role-load | always include guardrails/persona + active book's characters | safety, casting |
| Structured | frontmatter `type`/`tags` + link-follow | "all `Character` in this book" |
| Keyword | ripgrep / SQLite FTS5 | names, exact terms |
| Semantic | local embeddings (e.g. MiniLM) → in-memory cosine | paraphrase recall |

Per-turn cost = role-load + a local search (ms). No vector server / extraction LLM on the hot path
in the local design. **mem0 is an optional upgrade** behind the same `recall` call.

### 17.6 Where data lives (local-first / cloud-optional)
| Data | Default location | Cloud (opt-in, consented) |
|------|------------------|---------------------------|
| Working/episodic, recall index, curated OKF | **on-device, encrypted** | — |
| Heavy consolidation/extraction | on-device at idle | offload when online |
| Backup, parent dashboard, multi-device sync | — | encrypted cloud |

### 17.7 Book pipeline (offline, batch) — ⛔ planned
Not on the device hot path: `book text → identify speakers + attribute dialogue + tag emotion →
cast voices → pre-render expressive audio → cache`. Output: per-book OKF docs + a cached audio set
for offline playback. Heavy expressive TTS happens here, once (cloud).

---

## 18. Cross-Cutting Concerns

- **Real-time/threading.** Audio island: 5 ms quantum, lock-free, no blocking I/O, no malloc;
  workers pinned A76 SCHED_FIFO 89. Control plane: non-blocking mq poll + EventQueue single
  consumer + worker pool. No blocking call ever touches abox SPA threads.
- **Safety (defense in depth).** (1) OKF `Guardrails` → system prompt (editable layer). (2)
  Programmatic **output gate** in llm_connector before TTS (rules/classifier) → safe fallback on
  violation. Both layers wrap **both** LLM routes (local & cloud). ⛔ output gate not yet built.
- **Privacy & child-data compliance.** On-device by default; encryption at rest; verifiable
  parental consent for any cloud egress; data minimization + retention limits; parent view/erase.
  Designed to keep COPPA/GDPR-K exposure minimal by not shipping children's data off-device by
  default.
- **Security.** Localhost-only sidecar bindings; no inbound network on device by default; signed
  content bundles; cloud secrets provisioned not embedded; bus is local IPC.
- **Offline & degradation.** Cached books play offline; local recall works offline; cloud
  LLM/TTS/backup degrade to local/skip; missing sidecar → empty recall, agent still runs.
- **Observability.** One logging surface: `hermes/common/Log.h` (`HM_LOG_*` macros —
  TradingAlpha-compatible `YYYYMMDD HH:MM:SS:µs pid tid file:line <LEVEL> msg`;
  `HERMES_LOG_LEVEL` filter; new code never uses raw fprintf). Dev-only per-node trace:
  `HERMES_ABOX_TRACE=N` (enter/exit + per-node dt µs — NFR-8-unsafe, bring-up only).
  Buffering health: `AudioRing` available/high-water/overrun counters logged every 2 s +
  a per-turn `turn finalized: N ms decoded` summary. Plus abox `processed`/`drops`/
  Soft-Mute/Xrun counters and events; `gui_interface` live control + event feed in dev.

---

## 19. Deployment & Build

```
DEVICE (RK3588, Linux/PREEMPT_RT)
  PipeWire + WirePlumber · processes 1–8 · local OKF store (encrypted) · local index
  [optional local: small LLM (Ollama) · embeddings · mem0 sidecar :7070]
CLOUD (optional, consented)
  complex-LLM · book-prerender TTS · encrypted backup · parent web dashboard
```
- **Build:** root `CMakeLists.txt` (C11 RT + C++17 control), `enable_testing()`. `hermes_abox`
  built only if `libpipewire-0.3` present; tests built only if GTest present (offline-safe).
- **Cross-compile:** `cmake/aarch64-linux-gnu.toolchain.cmake` (generic) + `cmake/rk3588.toolchain.cmake`
  (`-mcpu=cortex-a76.cortex-a55`, GCC ≥10 / Clang ≥11). Docker: `ubuntu:24.04` + `g++-aarch64-linux-gnu`
  + `libpipewire-0.3-dev:arm64`.
- **Run/validate:** `scripts/build.sh {cross|native|run|test}`; `run_gui.sh`, `run_target.sh`,
  `run_loopback.sh`. `VALIDATION.md` = 6-step matrix; `SVVR.md` (Software Verification & Validation
  Report) = 11 ABOX sub-cases + 5 system tests.
- **Tests:** `abox_selftest` (12 suites incl. ABOX-11 bit-exact loopback + `test_capgate`),
  `test_eventmap`, `test_msgbus` (passing); `barge_in_e2e`, `kwd_wake_e2e` are `GTEST_SKIP`
  placeholders (SVVR A-1 — not passes). GoogleTest/CTest; sanitizers in CI.

---

## 20. Technology Choices & Rationale

| Area | Choice | Why |
|------|--------|-----|
| Knowledge format | **OKF** (md + YAML frontmatter) | standard, portable, readable, free visualizer |
| Curated retrieval | **local search over OKF** | offline, private, near-zero footprint |
| Dynamic memory engine | **optional mem0** behind facade | extraction/evolution if/when needed |
| LLM | **local-first, cloud-route** | latency/privacy local; capability on cloud |
| TTS | **pre-render (cloud) + cache; local for interactive** | expressive quality vs on-device cost |
| IPC | **POSIX-mq MsgBus** | proven, async, priority lanes |
| Audio | **PipeWire + C ABOX (Model B)** | zero-copy, RT, one filter hosts the graph |
| Dispatch | **EventMap (id→member) + EventQueue (3-lane)** | tiny, testable, single-consumer FSM |

---

## 21. Implementation Status (honest)

**Built ✅** *(updated 2026-07-20)*: IPC contract (MsgBus/CMsg/EventMap/EventQueue/Catalog),
Session FSM (states + transitions + threading; `SET_MODE` carries its body; wake body forwarded),
ABOX RT engine (graph incl. structural stages, routing matrix — SES column retired, vDMA,
buffer/worker pool, reference manager, param store, soft-mute), **DMX + CAPGATE nodes**
(`START/STOP_CAPTURE` handled), PipeWire hosting + locked 5 ms quantum, SRC node kernel,
**VTS wake word** (sherpa-onnx KWS + preroll-ring writer), **UC-12 capture path**
(`out_0` clean-feed capture via `HERMES_PW_CAP_TARGET`, `AudioRing` seam, resident streaming
STT with preroll backfill, `STT_FINAL` → story_agent, Groq cloud round-trip), `Log.h`
logging + `HERMES_ABOX_TRACE` node trace, story_agent basic loop, Memory facade + mem0
sidecar, gui_interface (incl. STT card), cross-compile + test harness, `run_voice.sh`.
*(UC-12 runtime validation on QEMU/target pending — code-complete, host-verified.)*

**Framework ⚠:** llm_connector answer path (Groq/Piper real, but playback bypasses abox —
`PLAYBACK_DRAINED` impersonated; abort not preemptive during curl/TTS; endpoint = RMS loop,
not decoder/ear-VAD; guardrail output gate missing; no `PLAY_SEGMENT` handler), AEC/BEAM
DSP kernels (passthrough stubs; AEC ref ring has no live far-end producer), story_agent
worker dispatch for `remember()`, FSM timeouts (`TO_*` unhandled — `SS_FAULT` is terminal).

**Planned ⛔:** `in_tts` playback port + TTSOUT duck (the barge-in prerequisite), VAD
barge-in (ear-process silero; the ≤12 ms KPI depends on it), guardrail output gate, local
OKF retrieval backend, consolidation job, `exportMd`/parent erase, cloud TTS/backup, book
pre-render integration (studio bundle → `PLAY_SEGMENT`), codec_hw, video_proc,
WirePlumber link policy on the target image, optional mem0-at-scale & multi-device sync.

---

## 22. Limitations, Risks & Open Decisions

1. **On-device footprint** of interactive LLM + STT + TTS + consolidation — **highest risk**;
   validate with a spike before committing model sizes.
2. **Barge-in is dormant end-to-end.** The duck is *designed* data-plane-local in ABOX and the
   supervisor's orchestration (LLM `ABORT` + capture restart on `MODE_CHANGED`) is built, but no
   component yet produces `AUDIO_CORE::evt::BARGE_IN` — the VAD node is net-new and the ≤12 ms KPI
   depends on it. PTT (`START_SESSION`) is likewise unhandled (a no-wake entry into a turn).
3. **DSP kernels are passthrough** — ABOX-11 asserts bit-exact today; the assertion must be
   updated when real AEC/BEAM land.
4. **mq depth = 10** (`kMqMaxMsg`, unprivileged limit) — verify under burst; URGENT lane protects
   barge-in but storms could drop DEFERRED.
5. **`remember()` currently blocks the turn** (heavy mem0 extraction) — must move off the IPC
   thread.
6. **Two FSMs** (session turn FSM vs story_agent state) coordinate via events, not a shared owner —
   document/guard the interaction (e.g. barge-in during narration).
7. **OKF v0.1 may churn**; **mem0 in/out** default *out* at scale; **consolidation trigger** (good
   night vs idle timer vs session end) — open.
8. **No init/systemd supervision** of the process fleet yet; launch is script-driven.
9. **Async-pool concurrency vs. stateful nodes (2026-07-20 review finding).** The async
   buffer pool runs 2 worker threads that can execute the SAME node instance's `process()`
   for two in-flight periods concurrently. CAPGATE's gain is atomic (fixed; residual = a
   rare transition-block ramp restart — the fully order-independent design is a
   position-anchored gain, noted in the node source); but **SRC's phase-carry/`last[]`
   state and AEC's mix ramp are plain fields** — a data race the moment those kernels do
   real work under the async path. Options when it matters: per-slot node state, a single
   graph-worker, or `HERMES_SYNC=1`. Benign today only because AEC is a stub and SRC runs
   the stateless ratio-1.0 fast path.

---

## 23. Phased Roadmap
- **P0 (done):** module skeleton on the bus (1–8), IPC contract, Session FSM, ABOX engine,
  story_agent loop, Memory facade + mem0 sidecar, OKF scaffold.
- **P1:** OKF conventions locked; local retrieval (keyword + structured); guardrail gate;
  llm_connector renders `PLAY_SEGMENT` end-to-end against stubs.
- **P2:** real STT + interactive LLM (local) + expressive TTS; episodic logging off-thread.
- **P3:** consolidation job (LLM→OKF); local semantic recall; book pipeline (cast/prerender).
- **P4:** VAD barge-in node (≤12 ms); cloud-optional (backup, dashboard, complex-LLM).
- **P5:** real AEC/BEAM kernels; optional mem0 at scale; multi-device sync.

---

## 24. Glossary & References

**OKF** Open Knowledge Format · **ABOX** the C audio engine (DSP island) · **Facade** the
`recall/remember/export` memory seam · **Quantum** the 5 ms / 240-sample RT period · **Soft-Mute**
zero-fill fallback on a missed DSP deadline · **Consolidation** idle LLM pass distilling episodic →
OKF · **CMsg** the control-plane wire message · **ModuleId** the process address on the bus ·
**VTS** voice-trigger service (wake word) · **vDMA** **virtual** DMA — the software ingress/egress
nodes (`abox_vdma.c`) moving samples between PipeWire port buffers and buffer-pool slots; the
engine's only host↔engine memory boundary. *(Not to be confused with Part II's historical "VDMA"
= Video DMA synchronization — see the Part II precedence note.)*

**Companion docs:** `memory_architecture.md`, `story_agent_SDS.md`, `VALIDATION.md`,
`SVVR.md`, `brain/README.md`, `brain/guardrails.md`. *(The former `AudioBox_DSP_Framework_SDS.md`
and `abox_pipewire.md` are now **Part II** and **Part III** of this document, below — §24 ends Part I.)*
**Source of truth:** `common/include/hermes/common/{Catalog,ModuleId,CMsg,MsgBus,EventMap,EventQueue,StoryMsg}.hpp`,
`app/supervisor/SessionFsm.*`, `app/audio_core/abox/*`, `app/audio_core/pipewire/*`,
`app/story_agent/main.cpp`, `app/llm_connector/main.cpp`, `services/memory/server.py`.


---

# Part II — AudioBox DSP Framework: Detailed Engineering Design (SDS)

> **Folded in (2026-06-29) from the former `AudioBox_DSP_Framework_SDS.md`** — the deep DSP-engine
> design for the *8-Core Embedded Linux (Yocto Target) Ultra-Low-Latency & High-Reliability AudioBox
> DSP Framework*. This part is the engine internals behind Part I §13 (node vtable, micro-scheduler,
> worker pool, lock-free async pipeline, watchdogs, scalable node framework, and the as-built
> realization of the C data plane).
>
> **Precedence / how to read this with Part I.** Where Part II overlaps Part I — the **command/event
> catalog, ModuleId set, Session-FSM states, and engine modes** — **Part I is authoritative**: it is
> verified line-by-line against the current code. Part II is retained *in full* for design rationale
> and depth, but it predates some renames, so it still shows a **6-module** control set with
> `CLOUD_CONNECTOR = 5` and **no** `STORY_AGENT(8)` / `GUI_INTERFACE(7)`. For the as-built addressing
> and catalog use **Part I §10 / §12**; Part II §17.3 carries the SDS's own reconciliation notes.
> **Section numbers below (§1–§18) are Part II-local** (inline `§N` references resolve within Part II).
> **Terminology caution — "VDMA":** in this Part's historical text (e.g. §1) **VDMA means *Video*
> DMA synchronization** (A/V lip-sync — the concern now parked in the `video_proc(4)` stub). In the
> as-built engine and everywhere in Part I, **vDMA means *virtual* DMA** — the software
> ingress/egress nodes moving samples between PipeWire buffers and buffer-pool slots
> (`abox_vdma.c`, Part I §24 glossary). Same letters, unrelated concepts; Part I's meaning governs.
> **v2 architecture changes** (supersede v1): (1) compute is **mode-adaptive** — the big cluster is only spun up when there is actual echo/streaming work, so the device idles cheaply in keyword-listening; (2) the worker pool uses a **hybrid park/spin** wake instead of unconditional busy-spin, so big cores sleep ~80% of the time; (3) AEC is **partitioned-block frequency-domain (PBFDAF)** with a real ~190 ms tail plus explicit reference/loopback delay alignment; (4) the engine is a **mode-selected processing graph (DAG)** of nodes with declared dependencies rather than a single hard-coded sequence.

> **As-built note:** §1–§17 are the design intent. For what is *actually implemented* today — the **C data plane** (`abox_node` vtable, mask-gated graph tick, reference manager, self-claim worker pool, param store), the integration decisions **D9–D12** (§17.1), and the **current buffering/overflow reality** (Soft-Mute, not yet the §10 multi-buffer) — see **§18**.

---

## 1. System Overview
This document establishes the Software Design Specification (SDS) for a hard real-time, high-reliability AudioBox DSP framework deployed on an embedded Linux environment built via the Yocto Project. The framework manages multi-channel microphone Beamforming, Asynchronous Sample Rate Conversion (SRC), Acoustic Echo Cancellation (AEC), Voice Activity Detection (VAD) for Barge-In handling, on-device Keyword Detection (KWD), and Video DMA (VDMA) synchronization. All components are architected for 8-core CPU configurations to guarantee zero-dropout (no glitching) audio stream execution under tight embedded constraints.

### 1.1 Core Architectural Principles
* **Two-Timescale Real-Time:** Two independent clocks govern the system. (a) The **audio block period is 5 ms** — every DMA cycle pipes in 240 samples @ 48 kHz, and the engine has the full 5 ms *until the next block is piped in* to complete processing (the hard deadline). (b) A separate **1 ms control micro-scheduler** (LP Core 4) runs hardware/clock/fader housekeeping at finer granularity. The 1 ms cadence is **not** the processing budget; the processing budget is 5 ms.
* **Zero-Allocation Core:** Heap allocations (`malloc`, `new`) are strictly prohibited inside the real-time audio threads to avoid unpredictable kernel scheduling delays. Static pre-allocated memory pools are utilized exclusively.
* **Lock-Free Communication:** Traditional blocking primitives (`pthread_mutex`, condition variables) are banned within real-time domains. Thread boundaries are traversed using C11 atomic primitives (`_Atomic`) and Single-Producer Single-Consumer (SPSC) lock-free ring buffers.
* **Cache Alignment:** To eliminate False Sharing across the 8-core architecture, critical processing queues, worker job nodes, and shared contexts are explicitly aligned to 64-byte hardware cache lines via `alignas(64)`.

---

## 2. Hardware Mapping & Thread Topology
The target SoC is a heterogeneous **big.LITTLE** 8-core part: **4 high-performance cores** for compute-bound hard real-time DSP, and **4 low-power cores** for periodic control plus latency-tolerant long-running work. Every thread is pinned by affinity so the audio data-path never migrates, never competes with background work, and the low-power cluster is free to clock down when idle.

### 2.1 High-Performance Cluster (Cores 0–3) — Hard Real-Time DSP
* **HP Core 0 — Pipeline Coordinator:** awakened by the ALSA DMA/I2S hardware IRQ. Executes the topological RT chain (SRC → AEC join → Beamform → Perception → Output) and dispatches the AEC fan-out.
* **HP Cores 1–3 — DSP Worker Pool:** parallel AEC adaptive filtering and other heavy SIMD/NEON workloads. One worker thread is pinned per core; together with the coordinator's own context they form an `NUM_DSP_CORES`-wide pool that **self-claims** jobs from a shared queue (§11.2). With a 2-mic array, AEC fans out by **channel × frequency sub-band** (e.g. 2 channels × 2 bin-ranges = 4 jobs) so all four big cores stay busy despite only two channels — full-width parallelism with no idle core and no per-core work assignment.

### 2.2 Low-Power Cluster (Cores 4–7)
* **LP Core 4 — 1ms Micro-Scheduler (soft RT):** hybrid hrtimer + `_mm_pause()` spin loop holding a 1ms cadence. Drives `/dev/input` polling, clock-drift PI estimation, and the dB volume fader. Placed on the LITTLE cluster to save power and to avoid stealing DSP cycles from the big cores. *(May be promoted to a big core only if measured jitter exceeds the soft-RT budget.)*
* **LP Cores 5–7 — Background Worker Pool (non-RT):** drain the lock-free `AudioEventBus` to execute the **long processing tasks** — cloud STT streaming, raw file serialization, I2C codec resets, and heavy KWD model post-scoring. These tolerate latency and must **never** block the RT chain.

### 2.3 Heterogeneous Scheduling & Power Policy
* Big cluster: `cpufreq` **performance** governor (locked max frequency) so DSP timing is deterministic; LITTLE cluster: **schedutil/powersave** so it idles between events.
* All RT threads run `SCHED_FIFO` and are excluded from Energy-Aware Scheduling (EAS) migration via hard affinity — the kernel never relocates a pinned DSP thread to a slower core mid-block.

---

## 3. Core Structural Declarations

```c
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdalign.h>

#define NUM_DSP_CORES 4      // big cluster: one DSP worker thread pinned per core (Cores 0–3)
#define MAX_WORKERS NUM_DSP_CORES
#define MAX_BKGD_WORKERS 3   // LP Cores 5–7 long-task background pool
#define MAX_CHANNELS 2       // 2-mic array (stereo capture); beamforms to 1 mono stream
#define MAX_JOBS 16          // upper bound on jobs a single parallel stage may emit
#define MAX_NODES 12         // processing-node registry capacity
#define MAX_NODE_INPUTS 4    // max upstream sources a single node can pull from
#define MODE_COUNT 4         // number of EngineMode graphs (one wiring per mode)
#define DSP_BLOCK_SAMPLES 240          // 5 ms @ 48 kHz — one DMA period piped in per cycle
#define MAX_BLOCK_SIZE 256             // buffer capacity (≥ block, padded for FFT framing)
#define BLOCK_DEADLINE_NS 5000000ULL   // 5 ms hard processing deadline = inter-block period
#define CTRL_TICK_NS      1000000ULL   // 1 ms control micro-scheduler cadence (LP Core 4)

typedef enum {
    HW_ERR_NONE = 0,
    HW_ERR_MIC_UNPLUGGED,
    HW_ERR_NETWORK_DISCONNECTED,
    HW_ERR_TIMEOUT_VIOLATION
} HardwareError;

typedef enum {
    MODE_KEYWORD_LISTENING = 0,
    MODE_BARGE_IN_MUTING,
    MODE_CLOUD_STREAMING,
    MODE_SYSTEM_RESET_ERROR
} EngineMode;

typedef struct {
    float pcm[MAX_CHANNELS][MAX_BLOCK_SIZE];
    int channelCount;
    int sampleCount;
    uint64_t hardwareTimestampNs;
} AdvancedAudioBlock;

// Aligned to 64-bytes to completely eliminate False Sharing during multi-core job distribution
typedef struct alignas(64) {
    void (*dspFunction)(void* context, int channel, int samples);
    void* contextData;
    int targetChannel;
    int sampleCount;
    int weightFactor; // Used for Dynamic Core Balancing (sorted from heaviest to lightest task)
    uint8_t padding[64 - ((sizeof(void*) * 2 + sizeof(int) * 3) % 64)]; 
} OptimizedAudioJob;

typedef struct alignas(64) {
    _Atomic EngineMode currentMode;
    _Atomic HardwareError lastError;
    _Atomic float clockDriftRatio;
    _Atomic float playbackVolume;
    _Atomic int activeJobCount;
    uint8_t padding[64 - ((sizeof(_Atomic EngineMode) + sizeof(_Atomic HardwareError) + sizeof(_Atomic float) * 2 + sizeof(_Atomic int)) % 64)];
} SharedFrameworkContext;

// TradingAlpha-style Lock-Free Async Messaging Bus
typedef enum {
    EVENT_KWD_DETECTED,
    EVENT_BARGE_IN_TRIGGERED,
    EVENT_HW_UNPLUGGED
} AudioEventType;

typedef struct alignas(64) {
    AudioEventType type;
    uint64_t timestampNs;
    uintptr_t payload;
    uint8_t padding[64 - (sizeof(AudioEventType) + sizeof(uint64_t) + sizeof(uintptr_t))];
} AudioEventMsg;
```

---

## 4. Component Processing Nodes (Internal State Design)

Every DSP module is a **`ProcessingNode`** implementing one **common interface** — a C-style OOP design (a function-table "class" + an opaque `state` "instance"). The engine never special-cases a node type; it holds an array of `ProcessingNode*` and drives them all through the same five entry points. This is what makes the pipeline readable, swappable, and uniformly schedulable. All node state is statically bound at compile-time (zero heap).

### 4.0 Common Node Interface (`ProcessingNode`)

```c
typedef struct ProcessingNode ProcessingNode;

// A node's output buffer (engine-owned static storage — nothing is allocated in a node).
typedef struct {
    float (*chan)[MAX_BLOCK_SIZE];   // channel-major sample buffer
    int      channelCount;
    int      sampleCount;
    uint64_t timestampNs;
} NodeBuffer;

// The "vtable": the common processing functions EVERY node implements.
typedef struct {
    const char* name;
    void (*prepare)(ProcessingNode* self, int sampleRate, int blockSize); // mode entry, one-shot
    void (*reset)  (ProcessingNode* self);                                // purge state (Reset Pipeline §6.2)
    // ASYNC-PIPELINE process: consume ONE input payload, produce ONE output payload
    // (zero-copy — buffers come from the static pool, §10.1). Return value = number of
    // parallel sub-jobs to fan across the pool (0 = ran inline on the calling core).
    int  (*process)(ProcessingNode* self,
                    const AudioBufferPayload* in, AudioBufferPayload* out);
    void (*fillJob)(ProcessingNode* self, int jobIdx, OptimizedAudioJob* job);
} NodeVTable;

// The "object": uniform handle. A node knows ONLY its own input queue and which queue
// it feeds — not who produced its input, not the global order. That locality is what
// makes wiring additive (insert a node = relink one `outQ` pointer, §10.3).
// (SpscPointerQueue / AudioBufferPayload are defined in §10.1.)
struct ProcessingNode {
    const NodeVTable* vt;        // class — static, shared by all instances of a type
    void*             state;     // instance — node-private state (statically allocated)
    SpscPointerQueue  inQ;       // this node's input ring (upstream's process_done() pushes here)
    SpscPointerQueue* outQ;      // → the downstream node's inQ (wiring; NULL at the sink)
    int               coreId;    // DSP core this pipeline stage is placed on (§10.4)
};

// Polymorphic call sites the engine uses — identical for every node type:
//   node->vt->prepare(node, 48000, DSP_BLOCK_SAMPLES);
//   int njobs = node->vt->process(node, inBuf, outBuf);
//   node->vt->reset(node);
```

Each concrete node (below) supplies a `static const NodeVTable` and a cast helper, e.g.:
```c
static int  Aec_Process(ProcessingNode* self, const NodeBuffer* const* in, int n, NodeBuffer* out);
static void Aec_Reset  (ProcessingNode* self);
static void Aec_Prepare(ProcessingNode* self, int sr, int bs);
static void Aec_FillJob(ProcessingNode* self, int i, OptimizedAudioJob* j);
const NodeVTable AEC_VTABLE = { "AEC", Aec_Prepare, Aec_Reset, Aec_Process, Aec_FillJob };
// helper: static inline AecNodeState* aec(ProcessingNode* s){ return (AecNodeState*)s->state; }
```

The subsections below specify only each node's **`state` struct** (the "member variables"); the four methods are the node's implementation of the contract above.

### 4.0.1 Global Common Services — `g_ac` Singleton
The functions that are **common to every node** — FFT/IFFT and SIMD math, parallel fan-out, event publishing, the monotonic RT clock, and access to the shared atomics — are *not* threaded through each `process()` call. They live in one **globally-accessible singleton** (a service locator), so any node reaches them directly via `g_ac->…`. This keeps node code terse and the vtable signature minimal, and gives every node a single, uniform way to call shared facilities.

```c
// One global instance, built ONCE at boot. After init its function pointers and
// `shared` handle are immutable — so concurrent read access from all DSP cores needs
// no lock (the only mutable shared state lives behind the atomics inside `shared`).
typedef struct {
    // --- common DSP kernels (NEON/SIMD), shared by SRC, AEC, Beamform, KWD … ---
    void  (*fft) (const float* in, float* out, int n);
    void  (*ifft)(const float* in, float* out, int n);
    float (*dotf)(const float* a, const float* b, int n);
    void  (*biquad)(float* x, const float* coeffs, int n);

    // --- engine facilities exposed to nodes ---
    bool      (*runParallel)(ProcessingNode* self, int njobs);  // §10.5 within-node fan-out
    void      (*publish)(const AudioEventMsg* msg);             // → AudioEventBus (LP Cores 5–7)
    uint64_t  (*now_ns)(void);                                  // monotonic hard-RT clock
    void      (*softMute)(NodeBuffer* out);                     // zero-fill fallback helper

    // --- shared control state (atomics: mode · drift · volume · error) ---
    SharedFrameworkContext* shared;
} AudioCommon;

extern AudioCommon* const g_ac;   // global service locator — every node uses this directly
```

Example — the AEC node reaches everything common through the singleton instead of parameters:
```c
static int Aec_Process(ProcessingNode* self, const NodeBuffer* const* in, int n, NodeBuffer* out) {
    AecNodeState* st = (AecNodeState*)self->state;
    g_ac->fft(in[0]->chan[0], st->/*…*/, AEC_FFT_SIZE);        // common math
    if (g_ac->now_ns() > /*…*/) { /* … */ }                    // common clock
    float drift = atomic_load(&g_ac->shared->clockDriftRatio); // shared atomics
    return njobs;   // engine calls g_ac->runParallel(self, njobs) via Node_Pull (§10.2)
}
```

> **Singleton discipline (RT-safe):** `g_ac` is write-once at boot and read-only thereafter; it holds **no mutable global state**, only stable function pointers and the `shared` atomics handle. That is what makes a global accessor safe on the lock-free hot path — there is nothing to serialize. Nodes must still never call a blocking facility through it.

### 4.1 SRC (Sample Rate Converter) Node — `SRC_VTABLE`
Compensates for the crystal clock drift between microphone input and speaker output streams.

```c
#define SRC_FILTER_TAPS 64
#define SRC_HISTORY_SIZE 512

typedef struct {
    float inputHistory[MAX_CHANNELS][SRC_HISTORY_SIZE];
    int writeIndex[MAX_CHANNELS];
    float filterCoefficients[SRC_FILTER_TAPS];
    double currentSamplePosition[MAX_CHANNELS];
    bool isFirstBlock;
} SrcNodeState;
```

### 4.2 Beamforming Node
Aggregates spatial multi-channel inputs down to a refined single-channel mono buffer.

```c
#define BEAM_FILTER_TAPS 32
#define BEAM_DELAY_MAX 16

typedef struct {
    float delayLines[MAX_CHANNELS][BEAM_DELAY_MAX];
    int delayWritePtr;
    int channelDelaySamples[MAX_CHANNELS];
    float spatialWeights[MAX_CHANNELS][BEAM_FILTER_TAPS];
    int targetAzimuthAngle;
} BeamformingNodeState;
```

### 4.3 AEC (Acoustic Echo Cancellation) Node — `AEC_VTABLE`
Cancels the loudspeaker signal — including its **room reverberation tail** — from the mic path. v2 replaces the v1 256-tap (5 ms) time-domain NLMS, which was both too short for a real room and too expensive, with a **Partitioned-Block Frequency-Domain (PBFDAF / multi-delay block) adaptive filter**. The echo tail is `AEC_PARTITIONS × 5 ms ≈ 200 ms`, matching real RT60 reverberation, while FFT-domain convolution keeps the MIPS far below an equivalent-length time-domain filter. Partitions and frequency-bin ranges are the **parallel unit**, so AEC fans out to fill all 4 big cores even with only 2 mics.

```c
// Partitioned-block frequency-domain AEC. Tail = AEC_PARTITIONS × block(5 ms).
#define AEC_PARTITIONS  40                 // 40 × 5 ms = 200 ms echo tail
#define AEC_FFT_SIZE    512                // overlap-save framing of the 240-sample block
#define AEC_BINS        (AEC_FFT_SIZE/2 + 1)

typedef struct alignas(64) {
    float W[AEC_PARTITIONS][AEC_BINS * 2];     // adaptive weights Ŵ_p(f), interleaved re/im
    float Xfifo[AEC_PARTITIONS][AEC_BINS * 2]; // delay line of reference spectra X_p(f)
    int   partHead;                            // ring head for the partitioned convolution
    float binPower[AEC_BINS];                  // smoothed |X(f)|² — MDF normalized step denom
    float mu;                                  // adaptation step (0 < mu < 2)
    float leak;                                // tap-leakage for ill-conditioned robustness
} AecChannelState;                             // one instance per mic channel

typedef struct {
    AecChannelState channel[MAX_CHANNELS];     // 2-mic array
    _Atomic bool    adaptFrozen;               // set by the Double-Talk Detector (see below)
} AecNodeState;
```

* **Double-Talk Detection (DTD):** when the near-end user is speaking *over* playback, adapting the filter would corrupt it. The DTD (driven by the VAD's near-end energy vs. the reference energy) sets `adaptFrozen`, holding the weights `W` steady while still subtracting the current echo estimate. This is the same residual-energy signal Barge-In uses (§8).
* **Residual Echo Suppression (RES):** a light post-filter on the AEC error spectrum removes the non-linear echo the linear filter cannot, before the signal reaches Beamforming.
* **vtable mapping:** `Aec_Process` runs the forward FFT, partitioned-convolution echo estimate, error/adaptation, and emits jobs (channel × bin-range); `Aec_FillJob` parameterizes one (channel, bin-range) slice; `Aec_Reset` zeros `W`/`Xfifo` for the Reset Pipeline (§6.2).

#### 4.3.1 Reference / Loopback Path (the far-end signal)
AEC only converges if it is fed the *actual* played-back samples, **time-aligned** to the mic. This node captures the post-mixer playback (the "far-end"), measures the bulk speaker→mic delay, and applies it before AEC; the §5 PI loop supplies the fine clock-drift for the ASRC stage that precedes it.

```c
#define REF_BULK_DELAY_MAX 4096            // ≤ ~85 ms speaker→mic acoustic + buffer delay search

typedef struct {
    float loopback[REF_BULK_DELAY_MAX];    // ring of captured playback samples (AEC far-end)
    int   writePtr;
    int   bulkDelaySamples;                // coarse delay from normalized cross-correlation
    bool  delayLocked;                     // true once the correlation peak is stable
} ReferencePathState;
```

#### 4.3.2 Reference Delay Management
AEC only cancels if the reference is **time-aligned to the echo** in the mic. A played sample `ref[k]` reaches the mic as `mic[k + D]`, where `D` is the round-trip transport delay. Managing `D` correctly is the single highest-leverage factor in real-world echo performance and barge-in reliability.

**(a) Delay anatomy & budget.** `D = D_playback + D_acoustic + D_capture` — dominated by buffering, *not* acoustics:

| Component | Typical | Source |
|-----------|---------|--------|
| Playback buffering (ALSA periods + DAC FIFO) | 20–40 ms | config-dependent, dominant |
| Acoustic (speaker→mic, ~0.3 m) | ~1 ms | ~3 ms / metre |
| Capture buffering (ADC + 1 period) | ~5 ms | |
| **Total `D`** | **~30–50 ms** | ≈ **480–800 samples @ 16 kHz** |

*(Sequential engine ⇒ no pipeline-depth term; if the pipeline path of §10 is ever used, add its depth to `D`.)*

**(b) Two-layer split — bulk delay + filter tail.** The adaptive filter is **not** stretched to cover transport delay + room tail. Instead:
* a **bulk delay** `D_bulk` (a coarse integer **ring-buffer read offset** — nearly free) removes the transport delay;
* the **PBFDAF filter** then models only the ~200 ms **room** response that follows.

This keeps the filter short → fewer MIPS, faster convergence, higher ERLE.

```c
#define REF_BULK_DELAY_MAX 1536                 // ≥ worst-case transport delay (~96 ms @ 16 kHz)

typedef struct {
    float    loopback[REF_BULK_DELAY_MAX];      // ring of POST-FADER far-end (what was emitted)
    uint32_t writePtr;                           // advanced as playback is captured
    int      bulkDelaySamples;                   // D_bulk — the integer "ref delay count"
    float    fracDelay;                          // sub-sample residual, tracked for drift
    bool     delayLocked;                        // cross-correlation lock state
    float    erleSmoothed;                        // health monitor → triggers re-lock
} ReferenceDelayState;

// write far-end each block (post-fader loopback, §10.6):
loopback[writePtr++ % REF_BULK_DELAY_MAX] = farend;
// read the aligned reference for AEC sample n (fractional interp tracks drift):
idx = (writePtr - bulkDelaySamples - (BLOCK - n)) % REF_BULK_DELAY_MAX;
ref_aligned[n] = interp(loopback[idx], fracDelay);
```

**(c) Estimating `D_bulk` — three stages.**
1. **Seed (timestamps):** `D ≈ mic.hwTimestampNs − ref.hwTimestampNs` (≈ `snd_pcm_delay`). Gets within a few ms instantly, no compute.
2. **Lock (normalized cross-correlation, during single-talk):** `D_bulk = argmaxₗ Σ ref[t−l]·mic[t] / (‖ref‖‖mic‖)`; set `delayLocked` when the peak is strong. This is the robust estimate.
3. **Verify (filter-energy centering, continuous):** after convergence, take the center-of-mass of `|adaptiveWeights|`; if it drifts toward the tail edge, nudge `D_bulk` to keep the echo centered in the filter window.

**(d) Tracking change — drift vs. jumps.**
* **Slow drift** (ADC↔DAC crystal skew, ppm): the §5 PI loop + ASRC continuously adjust `fracDelay` (sub-sample resample) — smooth, glitch-free.
* **Sudden jumps** (buffer under/overrun, output-route change, SR change): detected as an **ERLE drop** → re-run stage (2) cross-correlation lock.

**(e) Post-fader requirement.** The reference **must** be tapped *after* the volume/duck fader (§4.3.1, §10.6). During a barge-in duck the emitted echo is fading; referencing the pre-fader (full-level) signal would over-subtract and diverge at the worst moment. `D` handles *timing*; post-fader handles *amplitude* — both are required.

**Failure modes:** `D_bulk` too small → echo falls outside the filter window → poor cancellation → false barge-in; too large → filter models silence → divergence; no drift tracking → ERLE collapses over minutes; pre-fader tap → over-subtraction during ducking.

### 4.4 Perception (VAD / KWD) Node
Evaluates short-term frame energy to track vocal continuity (Barge-In) and performs localized 호출어 matching.

```c
#define VAD_ENERGY_WINDOW 64
#define KWD_FEATURE_FRAMES 50

typedef struct {
    float windowEnergyHistory[VAD_ENERGY_WINDOW];
    int windowPtr;
    float currentRunningEnergy;
    int speechPersistCountMs;
    int silencePersistCountMs;
    float featureMatrix[KWD_FEATURE_FRAMES]; 
    int featureWriteRow;
    float kwdConfidenceThreshold;
} PerceptionNodeState;
```

---

## 5. 1ms Micro-Scheduler & Asynchronous Event Isolation

### 5.1 1ms Scheduler Operation
To bypass kernel-level thread sleep inaccuracies, **LP Core 4** operates on a high-precision hybrid scheduling architecture (hrtimer kernel sleep + `_mm_pause()` CPU spin) holding a **1 ms control cadence** — distinct from and finer than the 5 ms audio block period. It executes three real-time control duties:
1. **Non-Blocking Linux `/dev/input` Polling:** Opens the kernel switch descriptor via `O_NONBLOCK`. Reads events natively inside the 1ms loop. If an uncoupling signal (`SW_HEADPHONE_INSERT` matching 0) is parsed, it shifts `currentMode` to `MODE_SYSTEM_RESET_ERROR` within microseconds.
2. **Clock Drift PI Calculation:** Measures sample variances inside the SPSC ring buffer. Evaluates errors using a Proportional-Integral (PI) control feedback loop combined with a Low-Pass Filter (LPF) to dynamically tune `clockDriftRatio`.
3. **Decibel Log-Scale Volume Fader:** Smooths volume adjustments during a Barge-In interrupt. Instead of a linear drop (which creates audible pops), it calculates attenuation using a dB logarithmic scale, achieving absolute silence within ~12ms.

### 5.2 Heavy Asynchronous Task Separation
Operations exceeding the 1ms hard deadline—such as cloud socket transmission (STT), raw file serialization, or I2C peripheral codec resets—are strictly forbidden from blocking real-time units.
* The system utilizes a **bounded, lock-free MPSC priority-lane trigger dispatcher** (§13) — not a single slot — so frequent async triggers (constant barge-in, TTS chunks, mode changes) are never dropped under burst.
* Real-time threads publish event packets via priority-tagged `Trigger_Post` in sub-microsecond intervals.
* Urgent triggers are serviced by the 1 ms scheduler (≤1 ms); deferred triggers are consumed by background workers, handling I/O and cloud transactions at their own pace without delaying the audio pipeline (§13.3).

---

## 6. Fault Tolerances & Dual-Layer Real-Time Watchdogs

### 6.1 Internal Layer: Job Dispatcher Watchdog
When distributing DSP jobs across HP Cores 1–3, Core 0 continuously tracks an ultra-fine nanosecond timer against the **5 ms block deadline** (`BLOCK_DEADLINE_NS`). If a helper core stalls due to system jitter and the block cannot complete before the next 5 ms frame is piped in, Core 0 triggers an immediate `forceAbort` atomic broadcast. The dispatcher abandons the computation, downscales the audio frame using zero-filled interpolation (`Soft Mute Fallback`), and rescues the hardware output ring buffer from underflow — preventing a system freeze.

### 6.2 External Layer: DMA Interval Watchdog & Reset Pipeline
If the driver-level ALSA DMA interrupt timing drifts beyond 1.5× its designated 5 ms cadence (i.e. > 7.5 ms between blocks), it signifies a severe system stall. The interval sentinel captures this deviation, blanks out the physical codec outputs to shield the hardware from loud popping noise, and kicks off the `Reset Pipeline`. The framework then purges internal filter states and recalibrates the hardware state machine before transitioning back to the `MODE_KEYWORD_LISTENING` baseline.

---

## 7. Keyword Detection (KWD)
On-device wake-word recognition is the framework's default entry trigger. It runs continuously inside the Perception Node (§4.4) while the engine is in `MODE_KEYWORD_LISTENING`, consuming the post-AEC / post-Beamforming mono stream so the detector sees the cleanest possible signal.

### 7.1 Detection Pipeline
1. **Feature Extraction:** Each ~10ms frame is reduced to a compact feature vector and pushed into the `featureMatrix` ring (`KWD_FEATURE_FRAMES` deep), forming a rolling ~500ms acoustic window.
2. **Scoring:** A statically-weighted, zero-allocation classifier evaluates the rolling window every frame and emits a confidence score in `[0.0, 1.0]`.
3. **Thresholding & Debounce:** A detection fires only when confidence exceeds `kwdConfidenceThreshold` for a minimum sustained run, suppressing single-frame false positives.
4. **Event Publication:** On a confirmed hit the RT thread publishes an `EVENT_KWD_DETECTED` packet onto the lock-free `AudioEventBus` (§5.2) in sub-microsecond time — it does **not** block on any downstream action.

### 7.2 State Transition
A confirmed keyword transitions the engine `MODE_KEYWORD_LISTENING → MODE_CLOUD_STREAMING`. The background consumer reacts to `EVENT_KWD_DETECTED` by opening the STT socket and beginning upstream transmission, while the RT pipeline keeps capturing without interruption.

```
[MODE_KEYWORD_LISTENING]
        │  confidence > threshold (sustained)
        │  publish EVENT_KWD_DETECTED
        ▼
[MODE_CLOUD_STREAMING] ──► background worker opens STT socket
```

---

## 8. Barge-In Handling
Barge-In lets the user interrupt active playback (e.g. a TTS response) simply by speaking. It is driven by the VAD half of the Perception Node and is intentionally decoupled from KWD so it can fire mid-utterance without a wake word.

### 8.1 Trigger Logic
1. **Energy Tracking:** The VAD maintains `currentRunningEnergy` over the `windowEnergyHistory` ring and increments `speechPersistCountMs` while frame energy stays above the speech floor (resetting via `silencePersistCountMs` on silence).
2. **Reference-Aware Gating:** Because AEC has already removed the loudspeaker's own echo from the mic path, residual sustained energy is attributed to genuine local speech rather than the device's playback — preventing the system from interrupting itself.
3. **Confirmation:** When `speechPersistCountMs` crosses the Barge-In dwell threshold, the RT thread publishes `EVENT_BARGE_IN_TRIGGERED`.

### 8.2 Pop-Free Ducking & State Transition
On a confirmed Barge-In the engine enters `MODE_BARGE_IN_MUTING`. The LP Core 4 micro-scheduler's **dB log-scale volume fader** (§5.1.3) attenuates playback to absolute silence within ~12ms — fast enough to clear the path for the user, smooth enough to avoid an audible pop. Once playback is muted and the user utterance completes, the engine routes the captured speech upstream (`MODE_CLOUD_STREAMING`) or returns to `MODE_KEYWORD_LISTENING`.

```
[playback active]
        │  speechPersistCountMs > dwell  (AEC-gated, echo removed)
        │  publish EVENT_BARGE_IN_TRIGGERED
        ▼
[MODE_BARGE_IN_MUTING] ──► dB fader → silence in ~12ms ──► capture user speech
```

---

## 9. Robust End-to-End Processing Sequence

### 9.1 Signal-Chain Ordering & Rationale
The order of DSP stages is not arbitrary — each stage depends on an invariant established by the one before it. Violating this order silently degrades every downstream block.

| Order | Stage | Depends on | Why this position is mandatory |
|-------|-------|------------|-------------------------------|
| 1 | **Mic Ingest** (ALSA DMA) | — | Raw multi-channel PCM + hardware timestamp enter here only. |
| 2 | **Reference SRC** (ASRC) | clock-drift ratio | AEC requires the loudspeaker reference to be **sample-aligned** to the mic clock. ASRC removes ADC↔DAC crystal drift *before* cancellation, or AEC diverges. |
| 3 | **AEC (per-channel)** | aligned reference | Runs **per mic channel** — before any mixing — so each mic's distinct echo path is modeled independently. Mixing first would destroy the per-path structure. |
| 4 | **Beamforming** | echo-free channels | Combines the now echo-cancelled channels into one enhanced mono stream. Doing this *after* AEC means the beamformer steers on speech, not on echo energy. |
| 5 | **VAD / Barge-In** | clean mono + AEC residual | Sustained residual energy after AEC is attributable to real local speech, not playback — this is what makes self-interruption impossible. |
| 6 | **KWD** | clean mono | Wake-word scoring sees the cleanest possible signal, maximizing detection rate / minimizing false accepts. |
| 7 | **Output Mix + dB Fader** | engine mode | Playback path applies the volume fader (ducking on Barge-In) immediately before the output DMA commit. |

**Invariant:** `Ingest → SRC → AEC → Beamform → (VAD ∥ KWD) → Output`. Perception (5,6) is read-only on the mono buffer and may run concurrently.

### 9.2 Per-Block Execution Timeline (logical order)
> **Note:** the sequence below is the **logical data-dependency order** for one block. The async pipeline (§10) executes these stages *overlapped across blocks* via SPSC queues (SRC of block *N* runs while AEC handles *N−1*) — but the ordering and invariants below are exactly what the queue wiring must preserve. Read this as "what depends on what," and §10 as "how it is scheduled."

```
T0  ALSA/I2S IRQ ──► Core 0 wakes  (240-sample / 5 ms block ready)
        │  capture hardwareTimestampNs, copy DMA → AdvancedAudioBlock
        ▼
[A] Reference SRC      (Core 0)        apply clockDriftRatio from LP Core 4 PI loop
        ▼
[B] AEC fan-out        (Cores 0–3)     emit jobs (channel × sub-band), bump generation,
        │                              sorted heaviest→lightest (weightFactor);
        │                              all 4 cores self-claim from the shared queue
        │  ── Core 0 joins drain, then spin-waits on jobsRemaining, 5 ms watchdog armed ──
        ▼
[C] AEC join / barrier (Core 0)        all jobs done, or forceAbort on deadline
        ▼
[D] Beamforming        (Core 0)        N channels → 1 enhanced mono buffer
        ▼
[E] VAD + KWD          (Core 0)        update energy/feature rings;
        │                              publish EVENT_* to AudioEventBus (non-blocking)
        ▼
[F] Output Mix + Fader (Core 0)        apply playbackVolume (dB log-scale)
        ▼
[G] Commit             (Core 0)        write output DMA ring  ── or Soft-Mute Fallback
```

### 9.3 Concurrent Execution Domains
Three domains run independently and communicate **only** through atomics and lock-free buffers — never through locks.

```
─ HIGH-PERFORMANCE CLUSTER (hard real-time) ────────────────────────────────
HP Core 0    ── RT chain (SRC→AEC join→Beamform→Perception→Output) ► out DMA  (hard 5 ms)
HP Cores 1–3 ── DSP worker pool, generation-counter wake, hybrid park/spin    (hard, per block)
─ LOW-POWER CLUSTER ────────────────────────────────────────────────────────
LP Core 4    ── 1ms loop: /dev/input poll · clock-drift PI · volume fader     (soft 1 ms)
LP Cores 5–7 ── AudioEventBus consumers: STT socket · file I/O · I2C reset    (non-RT, long tasks)
```

**Inter-domain contract:** the only data crossing cluster boundaries does so through `_Atomic` fields in `SharedFrameworkContext` (mode, drift, volume) and the SPSC ring / `AudioEventBus`. No lock, syscall, or cache-coherency stall ever couples a big core to a LITTLE core on the critical path.

### 9.4 Robustness Sequence (failure handling woven into the flow)
The sequence is "robust" because every stage has a bounded-time escape hatch, so a single stall can never freeze the pipeline:

1. **Per-block deadline (internal watchdog, §6.1):** if the AEC fan-out (`[B]`) overruns the 5 ms block deadline, Core 0 broadcasts `forceAbort`, abandons the partial result, and emits a **Soft-Mute Fallback** (zero-fill) at `[G]` — the output ring never underflows.
2. **Cadence deadline (external watchdog, §6.2):** if the DMA interrupt itself drifts >1.5× the 5 ms cadence (>7.5 ms), the interval sentinel blanks the codec and enters the **Reset Pipeline** → purge filter states → recalibrate HW → return to `MODE_KEYWORD_LISTENING`.
3. **Hardware faults:** `/dev/input` unplug events (Core 7) set `lastError` + `MODE_SYSTEM_RESET_ERROR` within microseconds, short-circuiting the chain to a safe muted state.
4. **Back-pressure isolation:** any heavy/blocking work (cloud STT, serialization, codec reset) is published to the `AudioEventBus` and drained by background workers, so the RT chain's wall-clock is independent of network or filesystem latency.

### 9.5 Yocto / Platform Integration Requirements
The deterministic sequence above only holds if the embedded Linux image is configured for hard real-time. Required Yocto build inputs:

* **Kernel:** `PREEMPT_RT` patchset (`linux-yocto-rt`) for bounded scheduling latency; high-resolution timers enabled.
* **CPU isolation (big.LITTLE):** kernel cmdline `isolcpus=0-4 nohz_full=0-4 rcu_nocbs=0-4 irqaffinity=5-7` — the 4 big DSP cores (0–3) and the 1ms scheduler core (4) are removed from the general scheduler; RCU callbacks and device IRQs are pushed onto the LITTLE background cores (5–7). The ALSA/I2S audio IRQ is the exception: it is affined to HP Core 0.
* **Thread setup:** `SCHED_FIFO` priority + `pthread_setaffinity_np` pinning per the §2 topology; `mlockall(MCL_CURRENT|MCL_FUTURE)` to prevent paging of the static pools.
* **ALSA:** `meta-alsa` with the I2S/codec machine driver; **2-channel capture**, period size = **240 samples (5 ms)** at 48 kHz, to land the 5 ms DMA cadence.
* **Image recipe:** the framework packaged as its own recipe (`audiobox-dsp.bb`) with a `systemd` RT service unit (`CPUSchedulingPolicy=fifo`), depending on `alsa-lib`, the RT kernel, and the codec firmware.
* **Toolchain:** C11 (`_Atomic`, `stdalign`) with `-O3 -march=<soc>`; NEON/SIMD enabled for the AEC adaptive-filter inner loop.

---

## 10. Audio Processing Engine (Asynchronous Ring-Buffered Pipeline)
The engine is an **asynchronous ring-buffered pipeline** — the model used by hardware media accelerators (NXP / TI / Xilinx DSP-NPU IP) and high-performance media frameworks (GStreamer), and a strict evolution of a synchronous for-loop chain. Nodes are connected by **lock-free SPSC pointer queues**. A node finishes its block, **hands the buffer pointer to the next node (zero-copy) via `process_done()`**, and then — without waiting on the main core — immediately pre-processes the *next* incoming buffer (`source_update()`). Stages overlap across blocks like a hardware pipeline: while AEC works on block *N−1*, SRC is already crunching block *N*.

Two properties make this real-time safe: **zero-copy** (only the `AudioBufferPayload*` address moves between nodes, never the sample matrix), and **lock-free SPSC** (queues run on `_Atomic` head/tail only — no mutex ever touches the audio path). The node-graph scalability from earlier still holds: wiring is just connecting one node's `outQ` to the next node's `inQ`, so **adding a node never changes the engine** (§10.3).

### 10.1 Zero-Copy Payload, Static Pool & Lock-Free SPSC Queue
The data unit that flows through the pipeline is `AudioBufferPayload`; instances live in a **compile-time-allocated pool** (no heap, ever). Between every pair of nodes sits a 64-byte-aligned SPSC pointer queue sized for double/triple buffering (`QUEUE_CAPACITY`), which absorbs per-stage jitter.

```c
#define QUEUE_CAPACITY 4     // double/triple-buffer margin; absorbs a stage's transient overrun

// 1) The zero-copy audio buffer (the only thing that holds samples; sized for FFT framing).
typedef struct {
    float pcm[MAX_CHANNELS][MAX_BLOCK_SIZE * 2];
    int   channelCount;
    int   sampleCount;
} AudioBufferPayload;

// 2) Lock-free SPSC pointer queue — only ADDRESSES cross between nodes (zero-copy).
typedef struct alignas(64) {
    AudioBufferPayload* ring[QUEUE_CAPACITY];
    _Atomic int head;                          // producer cursor
    _Atomic int tail;                          // consumer cursor
    uint8_t pad[64 - ((sizeof(void*) * QUEUE_CAPACITY + sizeof(_Atomic int) * 2) % 64)];
} SpscPointerQueue;

static inline bool spsc_push(SpscPointerQueue* q, AudioBufferPayload* buf) {
    int h = atomic_load_explicit(&q->head, memory_order_relaxed);
    int t = atomic_load_explicit(&q->tail, memory_order_acquire);
    if ((h + 1) % QUEUE_CAPACITY == t) return false;          // FULL → overflow guard (§10.5)
    q->ring[h] = buf;
    atomic_store_explicit(&q->head, (h + 1) % QUEUE_CAPACITY, memory_order_release);
    return true;
}
static inline AudioBufferPayload* spsc_pop(SpscPointerQueue* q) {
    int t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    int h = atomic_load_explicit(&q->head, memory_order_acquire);
    if (t == h) return NULL;                                  // EMPTY → underflow guard (§10.5)
    AudioBufferPayload* buf = q->ring[t];
    atomic_store_explicit(&q->tail, (t + 1) % QUEUE_CAPACITY, memory_order_release);
    return buf;
}
```

A small **static buffer pool** hands out free payload slots; in production the ALSA DMA region is mmap'd straight onto this pool (§10.6) so even capture is copy-free.

```c
static AudioBufferPayload gPool[MAX_NODES * QUEUE_CAPACITY];   // physically allocated at build
static _Atomic int        gPoolNext;
static inline AudioBufferPayload* Pool_Acquire(void) {
    int i = atomic_fetch_add_explicit(&gPoolNext, 1, memory_order_relaxed);
    return &gPool[i % (MAX_NODES * QUEUE_CAPACITY)];           // ring reuse; sized so live set never wraps
}
```

### 10.2 Node Async Interface — `source_update()` / `process()` / `process_done()`
Every node exposes the same three pipeline hooks (on top of the §4.0 vtable). `source_update()` injects a fresh input pointer into the node's `inQ`; `process()` consumes one input and produces one output; `process_done()` pushes the finished pointer to the **next** node's queue in a few nanoseconds and returns — freeing the node to pipeline ahead.

```c
// Upstream (or the ALSA head) injects a new input buffer for this node.
static inline bool Node_SourceUpdate(ProcessingNode* n, AudioBufferPayload* in) {
    return spsc_push(&n->inQ, in);            // returns false if this node is backed up (§10.5)
}

// Hand the just-computed buffer to the DOWNSTREAM node and return immediately.
static inline void Node_ProcessDone(ProcessingNode* n, AudioBufferPayload* done) {
    if (n->outQ && !spsc_push(n->outQ, done)) // sink has no outQ
        Pipeline_SignalOverflow(n);           // next stage is full → recover, never block (§10.5)
}

// Run one pipeline step for node `n`: pop input, compute, publish. Non-blocking.
static void Node_RunOnce(AudioEngine* eng, ProcessingNode* n) {
    AudioBufferPayload* in = spsc_pop(&n->inQ);
    if (!in) { Pipeline_SignalUnderflow(eng, n); return; }    // nothing ready (warm-up / stall)

    AudioBufferPayload* out = Pool_Acquire();
    int njobs = n->vt->process(n, in, out);                   // §4.0 vtable: zero-copy in→out
    if (njobs > 0) Engine_RunParallel(eng, n, njobs);         // intra-node fan-out (§10.7)

    Node_ProcessDone(n, out);                                 // → next node, in ~ns
    // n is now free to immediately Node_RunOnce() again on the NEXT queued input → pipelining.
}
```

The SRC node makes the pipelining concrete — it publishes and then returns to pre-compute the next buffer without ever waiting on a downstream:

```c
int Src_Process(ProcessingNode* self, const AudioBufferPayload* in, AudioBufferPayload* out) {
    float drift = atomic_load(&g_ac->shared->clockDriftRatio);   // common services via singleton (§4.0.1)
    raw_src_algorithm(in, out, drift);                           // heavy resample → `out`
    return 0;                                                     // ran inline; no sub-jobs
}
// ...Node_ProcessDone() pushes `out` to AEC's inQ in ns, and SRC loops to the next block.
```

### 10.3 Wiring = Connect `outQ → next.inQ` (still no engine rewiring)
Inserting a node is one pointer relink — the producer/consumer code, the queues' lock-free contract, and the dispatcher are all untouched:

```c
ProcessingNode* ns = Engine_Register(eng, &NS_VTABLE, &g_nsState);  // engine just stores it
ns->outQ        = aecNode->outQ;     // NS now feeds whatever AEC used to feed (Beamform)
aecNode->outQ   = &ns->inQ;          // AEC now feeds NS
// Node_RunOnce, source_update/process_done, the pool, the watchdog: ALL unchanged.
```

### 10.4 Coupling with the 8-Core Job Dispatcher (2-D parallelism)
The pipeline gives **inter-stage** parallelism (different nodes on different cores, decoupled by queues); the §11.2 self-claim pool gives **intra-stage** parallelism (one heavy node split across cores). Combined, the AudioBox runs a 2-D parallel schedule:

* **Pipeline placement:** each node's `coreId` pins its stage to a DSP core. Light stages (SRC, VAD, KWD) are chained on one core; the heavy stage (AEC) owns the rest. A core's worker loop simply: *for each stage I own, `Node_RunOnce()` whenever its `inQ` is non-empty.*
* **Data-flow trigger:** a producer's `process_done()` bumps the consumer queue; the consumer core (parked via §11.2 hybrid park/spin) is woken by the same `generation`/futex mechanism — so a node runs the instant its input lands, not on a polled schedule.
* **Intra-node burst:** when a placed node (AEC) returns `njobs>0`, it borrows the whole self-claim pool for that step (channel × sub-band), then releases it back to pipeline mode.

```c
// Each DSP core runs this; nodes are assigned by coreId. Hybrid park/spin (§11.2) wakes on data.
static void* PipelineCore_Main(AudioEngine* eng, int coreId) {
    Thread_Pin(coreId, SCHED_FIFO, 89);
    for (;;) {
        for (int i = 0; i < eng->nodeCount; ++i)
            if (eng->nodes[i].coreId == coreId && QueueNonEmpty(&eng->nodes[i].inQ))
                Node_RunOnce(eng, &eng->nodes[i]);
        Core_ParkUntilData(eng, coreId);   // 0% CPU until a producer pushes to one of my queues
    }
}
```

### 10.5 Overflow / Underflow Recovery → 1 ms Scheduler Broadcast
The bounded queues turn a stall into a *recoverable event* instead of a crash. Both edges route to the LP Core 4 1 ms scheduler, which owns recovery:

* **Overflow** (`spsc_push` returns false — a consumer stage fell behind and its `inQ` is full): the producer does **not** block. It drops the frame to a **Soft-Mute Fallback**, sets `lastError = HW_ERR_TIMEOUT_VIOLATION`, and publishes `EVENT_*` to the `AudioEventBus`. The 1 ms scheduler broadcasts the condition and, if it persists, triggers the Reset Pipeline (§6.2).
* **Underflow** (`spsc_pop` returns NULL at the sink — no buffer ready when the output DMA needs one): the output stage emits a zero-filled block (no pop/click) and signals the scheduler so it can re-prime the pipeline.

```c
void Pipeline_SignalOverflow(ProcessingNode* n) {
    atomic_store(&g_ac->shared->lastError, HW_ERR_TIMEOUT_VIOLATION);
    AudioEventMsg m = { .type = EVENT_HW_UNPLUGGED /*=QUEUE_STALL*/, .timestampNs = g_ac->now_ns() };
    g_ac->publish(&m);                          // → LP Core 4 broadcasts / arms Reset Pipeline
}
```

Because `QUEUE_CAPACITY` provides several blocks of slack, a *transient* burst on one stage is simply absorbed — the producer keeps pre-computing ahead and the consumer catches up — and only a *sustained* stall escalates to recovery.

### 10.6 ALSA mmap — Zero-Copy from Driver to Pool (Yocto)
To remove even the capture `memcpy`, the ALSA capture buffer is `mmap`'d (`SNDRV_PCM_INFO_MMAP`) directly over the static payload pool, so the I2S DMA writes land *inside* an `AudioBufferPayload`. The head node's `source_update()` then just publishes that mmap'd pointer into the pipeline — no copy anywhere from codec to first node.

```c
// At init: map the ALSA mmap region onto the pool so DMA fills payloads in place.
snd_pcm_mmap_begin(pcm, &areas, &offset, &frames);
gPool[k].pcm[...] ⇐ areas[ch].addr + offset;   // pool slot k aliases the DMA area (no copy)
// Per period: snd_pcm_mmap_commit() advances; Node_SourceUpdate(head, &gPool[k]) publishes the pointer.
```

### 10.7 Within-Node Parallel Fan-Out
When a placed node's `process()` returns `njobs > 0`, `Engine_RunParallel` splits that one step across the self-claim pool (§11.2), barriering under the 5 ms deadline before `process_done()` publishes:

```c
static bool Engine_RunParallel(AudioEngine* eng, ProcessingNode* n, int njobs) {
    WorkerPool* p = eng->workers;
    for (int i = 0; i < njobs; ++i) n->vt->fillJob(n, i, &eng->jobScratch[i]);
    p->jobs = eng->jobScratch; p->jobCount = njobs;
    atomic_store_explicit(&p->nextJob, 0, memory_order_relaxed);
    atomic_store_explicit(&p->jobsRemaining, njobs, memory_order_relaxed);
    atomic_fetch_add_explicit(&p->generation, 1, memory_order_release);   // wake + futex_wake pool
    Pool_Drain(p);
    while (atomic_load_explicit(&p->jobsRemaining, memory_order_acquire) > 0) {
        if (g_ac->now_ns() > eng->blockDeadlineNs) {                      // 5 ms watchdog (§6.1)
            atomic_store_explicit(&eng->forceAbort, true, memory_order_relaxed);
            return false;
        }
        _mm_pause();
    }
    return true;
}
```

### 10.8 Engine Contracts & The Latency Trade-Off
* **Zero-copy everywhere:** only `AudioBufferPayload*` moves between stages; the sample matrix never moves. Inter-core "transfer" is a pointer write — no physical transfer latency.
* **Lock-free, mutex-free:** every hand-off is SPSC `_Atomic` head/tail; no kernel primitive on the audio path.
* **Adding a node = relink one `outQ`:** the dispatcher, queues, and recovery are component-agnostic.
* **Bounded, recoverable jitter:** `QUEUE_CAPACITY` absorbs transient per-stage overruns; only sustained overflow/underflow escalates to the 1 ms scheduler and Reset Pipeline.
* **⚠ Latency cost (design note):** asynchronous pipelining trades latency for throughput — end-to-end delay ≈ *(pipeline depth) × 5 ms* plus queue slack, versus one block for a synchronous chain. With ~4 stages that is ~20 ms, acceptable for KWD/streaming but it **must be folded into the AEC reference delay** (§4.3.1): the loopback alignment has to account for the pipeline depth, or AEC will diverge. Barge-In responsiveness (§8) is likewise measured from the *output* stage, not capture. **This latency only materializes when the scheduler actually pipelines across cores — which §10.9 makes the exception, not the default.**

### 10.9 Adaptive Locality-First Scheduling (the default execution policy)
Forcing each stage onto a dedicated core (fixed `coreId` placement) makes **every stage hand-off a cross-core migration**: the buffer leaves the producing core's L1/L2 and the consuming core must pull it back across the cache-coherency fabric — *every stage, every block* — plus it pays `depth × 5 ms` latency. That cost is only worth paying when the chain genuinely does not fit on one core. So the **default is sequential, single-core, run-to-completion; parallelism is opportunistic.**

**The three-tier policy (in priority order):**

1. **Sequential on one core (default).** The whole active chain runs **inline, in order, on a single big core** (the core woken by the capture IRQ). The buffer stays **cache-hot** from SRC → AEC → Beamform → Perception → Output; **no migration**, and latency is **one block (5 ms)**, not `depth × 5 ms`. This is the common case — especially `MODE_KEYWORD_LISTENING`, which always runs here while the other three cores stay parked.

2. **Heavy-node spill (only that node).** When a node's `process()` returns `njobs > 0` — meaning it alone won't fit the per-core budget — the engine borrows **idle** cores via the self-claim pool (§10.7) **for that node's step only**, then they re-park. Migration is confined to that one heavy node's slices (e.g. AEC channel × sub-band), not imposed on the whole chain.

3. **Opportunistic downstream offload (only to idle cores).** While running the sequential chain, *if and only if* other cores are idle **and** the remaining work risks the 5 ms deadline, the engine hands a **downstream sub-chain** to a free core via a queue push — gaining pipeline overlap at the cost of exactly one migration. If no core is idle, it never offloads — it stays sequential and cache-local.

```c
// Locality-first: run the chain on THIS core; spill only when heavy or when a core is free & we're at risk.
static void Pipeline_RunAdaptive(AudioEngine* eng, ProcessingNode* n, AudioBufferPayload* buf) {
    while (n) {
        AudioBufferPayload* out = Pool_Acquire();
        int njobs = n->vt->process(n, buf, out);                 // (1) inline on THIS core — cache-hot
        if (njobs > 0)
            Engine_RunParallel(eng, n, njobs);                   // (2) heavy → borrow IDLE cores, then re-park
        else if (Pool_IdleCoreCount(eng) > 0 && Chain_AtRisk(eng, n))
            { Offload_Downstream(eng, n->next, out); return; }   // (3) free core + at risk → pipeline the rest
        buf = out;                                               // otherwise keep going sequentially
        n   = n->next;
    }
}
```

**Idle detection** is cheap: the worker pool already knows how many cores are futex-parked (§11.2), so `Pool_IdleCoreCount()` is a single atomic read. **`Chain_AtRisk`** compares elapsed time against `blockDeadlineNs` discounted by the estimated cost of the remaining nodes.

**Consequences:**
* **Zero migration in the common case** — the chain is cache-resident on one core; cross-core coherency traffic only happens when work is actually spread.
* **Lowest latency by default** — sequential = 1-block latency; the `depth × 5 ms` pipeline latency is incurred *only* when tier 3 fires.
* **Power-aligned** — idle cores stay parked (0% CPU) unless heavy work or deadline pressure pulls them in, which is precisely the big.LITTLE intent (§2.3, §11.6).
* **`coreId` is now a hint, not a mandate** — it suggests *where* an offloaded stage prefers to land, used only by tiers 2–3; the default path ignores it.

### 10.10 Dispatcher / Load-Balancer Head Stage
The §10.9 policy is not engine-embedded logic — it lives in a **node**: the **first stage of every graph is a `Dispatcher` (load-balancer) node**. It consumes the capture buffer, reads live per-core load, and **routes the downstream chain** accordingly. Because it is an ordinary `ProcessingNode` (§4.0), the scheduling strategy is itself pluggable and swappable like any other node — you can replace the balancer without touching the engine.

```
[ALSA capture] ──▶ [ Dispatcher / Load-Balancer ]  ◄── owns core-load state + routing policy
                          │  decides per block:
            ┌─────────────┼──────────────────────────┐
            ▼             ▼                          ▼
   all cores busy   free core(s)              heavy node
   → SEQUENTIAL     → BALANCE downstream      → FAN-OUT that
     on one core      stages onto idle cores    node across cores
```

```c
typedef struct {
    AudioEngine*    eng;
    ProcessingNode* chainHead;                 // first real DSP node (e.g. SRC)
    int             coreLoadEwma[NUM_DSP_CORES];// smoothed measured cost per core (ns)
} DispatcherState;

// Head stage: read load, pick a distribution, drive the rest of the chain. Emits no jobs itself.
static int Dispatcher_Process(ProcessingNode* self, const AudioBufferPayload* in, AudioBufferPayload* out) {
    DispatcherState* d = (DispatcherState*)self->state;
    int idle = Pool_IdleCoreCount(d->eng);                 // single atomic read of parked workers (§11.2)

    if (idle == 0)                                         // cores all busy → cheapest path
        Pipeline_RunSequential(d->eng, d->chainHead, in);  //   stay on one core, cache-hot, no migration
    else
        Pipeline_RunBalanced(d->eng, d->chainHead, in, idle); // spread heavy/parallel stages onto idle cores

    return 0;   // the dispatcher ORCHESTRATES; it does not itself fan out
}
const NodeVTable DISPATCHER_VTABLE = { "dispatch", Dispatcher_Prepare, Dispatcher_Reset,
                                       Dispatcher_Process, /*fillJob=*/NULL };
```

**Load metric & balancing.** The dispatcher keeps an EWMA of each core's recently measured stage cost (`coreLoadEwma`, fed back after every node runs). When it offloads, it picks the **least-loaded idle core** (classic min-load assignment), so work spreads evenly instead of piling onto core 0. Heavy nodes still fan out via the self-claim pool (§10.7), which is inherently balanced by `fetch_add` claiming.

```c
// Walk the chain; for each node decide inline vs. offload-to-least-loaded-idle-core.
static void Pipeline_RunBalanced(AudioEngine* eng, ProcessingNode* n, const AudioBufferPayload* in, int idle) {
    const AudioBufferPayload* buf = in;
    while (n) {
        AudioBufferPayload* out = Pool_Acquire();
        int njobs = n->vt->process(n, buf, out);
        if (njobs > 0)                         Engine_RunParallel(eng, n, njobs);          // heavy → pool
        else if (idle > 0 && Node_IsOffloadable(n) && Chain_AtRisk(eng, n)) {
            int core = Pool_LeastLoadedIdleCore(eng);                                       // balance
            Offload_Chain(eng, n->next, out, core); return;                                 // hand rest off
        }
        Dispatcher_FeedbackLoad(eng, n);       // update coreLoadEwma with measured cost
        buf = out; n = n->next;
    }
}
```

**Why the head stage:**
* **Single decision point** — exactly one node owns "where does work run," so the policy is centralized, observable, and testable; the rest of the chain stays pure DSP.
* **Pluggable strategy** — swap `DISPATCHER_VTABLE` (round-robin, min-load, deadline-aware, power-capped) without touching any DSP node or the engine.
* **Mode-aware** — in `MODE_KEYWORD_LISTENING` the dispatcher simply always chooses sequential (idle cores stay parked); under streaming load it balances. Same node, different routing by mode.
* **Consistent with the framework** — the balancer is registered, arena-allocated, and wired exactly like every other node (§12); it is just the one wired at the head of each `ModeGraph`.

### 10.11 Per-Core Pipeline Queues — Directed Enqueue
The dispatcher routes work by **enqueuing a job into a specific core's own pipeline queue**. Every DSP core owns one input queue (`CorePipe`); the dispatcher is the producer, the core's thread is the consumer. This is *directed* placement (the balancer chooses the core), as opposed to the undirected self-claim pool (§10.7) which stays reserved for splitting a single heavy node.

```c
// One unit of pipeline work: run `node` (a stage or a sub-chain) on buffer `in`.
typedef struct { ProcessingNode* node; AudioBufferPayload* in; } PipeJob;

// Each DSP core owns an input pipeline queue (cache-line isolated). Dispatcher → core.
typedef struct alignas(64) {
    PipeJob          ring[QUEUE_CAPACITY];
    _Atomic int      head, tail;          // SPSC: dispatcher pushes, this core pops
    _Atomic uint32_t gen;                 // futex wake target
} CorePipe;

static CorePipe gCorePipe[NUM_DSP_CORES];

// Dispatcher places a job onto a chosen core's pipeline (the "enqueue to that core").
static void CorePipe_Enqueue(int coreId, ProcessingNode* node, AudioBufferPayload* in) {
    PipeJob j = { node, in };
    pipe_push(&gCorePipe[coreId], j);                 // lock-free SPSC push
    atomic_fetch_add_explicit(&gCorePipe[coreId].gen, 1, memory_order_release);
    futex_wake(&gCorePipe[coreId].gen, 1);            // wake that core if parked (§11.2)
}
```

**Core thread loop** now drains *its own* pipeline queue (this supersedes the node-scan in §10.4 — a core no longer polls all nodes, it consumes its queue):

```c
static void* PipelineCore_Main(AudioEngine* eng, int coreId) {
    Thread_Pin(coreId, SCHED_FIFO, 89);
    CorePipe* mine = &gCorePipe[coreId];
    for (;;) {
        PipeJob j;
        if (pipe_pop(mine, &j)) {                     // 1) my own queued work first
            AudioBufferPayload* out = Pool_Acquire();
            int njobs = j.node->vt->process(j.node, j.in, out);
            if (njobs > 0) Engine_RunParallel(eng, j.node, njobs);   // heavy → borrow shared pool
            Node_Route(eng, j.node->next, out);       // chain: enqueue next stage to a core (often same)
        } else if (!CorePipe_StealInto(coreId)) {     // 2) idle → optionally steal a job from a busy core
            Core_ParkUntilData(mine);                 // 3) nothing to do → futex-park at 0% CPU
        }
    }
}
```

**How the three tiers of §10.9 map to enqueue targets:**
| Tier | Dispatcher action |
|------|-------------------|
| Sequential (default, cores busy) | enqueue the **whole chain head** to **one** core; `Node_Route` keeps re-enqueuing `next` to the **same** core → stays cache-local, no migration |
| Balanced (idle cores free) | enqueue downstream stages to **different (least-loaded) cores'** pipes → inter-stage parallelism, one migration per offloaded stage |
| Heavy node (`njobs>0`) | the running core fans that node across the **shared self-claim pool** (§10.7), independent of the per-core pipes |

* **Directed vs. self-claim, together:** per-core pipes give the *balancer* explicit control of placement (locality, power, deadline); the shared pool gives a *single heavy node* undirected width. The two coexist — a core pops a directed `PipeJob`, and if that job is heavy it momentarily pulls the whole pool.
* **Work-stealing as the safety valve:** if the balancer misjudges and a core empties while another is backed up, `CorePipe_StealInto` lets the idle core take a job from the busy core's queue tail — so a mis-balance self-corrects instead of stalling.
* **Wake is per-queue:** enqueuing bumps that core's `gen` and `futex_wake`s only *that* core — a parked core runs the instant the dispatcher hands it a job, and stays parked otherwise (the §11.2 power model, now per-core-queue).

---

## 11. Multicore / Multithreading Runtime
This section specifies the threading substrate that the engine (§10) and the parallel fan-out (§10.5) run on. The AudioBox is explicitly multi-threaded across both clusters; the design rule is **one pinned thread per core, never more**, so the kernel scheduler is taken out of the critical path entirely.

### 11.1 Thread Inventory

| Thread | Core | Sched policy | Priority | Wake source | Duty |
|--------|------|--------------|----------|-------------|------|
| `pipeline_core[0]` (head) | HP 0 | `SCHED_FIFO` | 90 (highest) | ALSA DMA IRQ + queue data | head stages (Capture/SRC) + output commit |
| `pipeline_core[1..N-1]` | HP 1..N-1 | `SCHED_FIFO` | 89 | queue push / `generation` futex | placed pipeline stages (AEC…) + self-claim bursts |
| `micro_scheduler` | LP 4 | `SCHED_FIFO` | 80 | hrtimer (1ms) | `/dev/input`, drift PI, fader |
| `bkgd_worker[0..2]` | LP 5–7 | `SCHED_OTHER` | nice 0 | `AudioEventBus` | STT, file I/O, codec reset (long tasks) |

RT priorities sit **below** the kernel's IRQ-thread priority so DMA delivery is never starved; background workers are ordinary `SCHED_OTHER` so they yield to everything RT.

### 11.2 Worker Pool — one thread per core, shared-queue self-claim
The pool runs **`NUM_DSP_CORES` execution contexts, one pinned per big core**. `NUM_DSP_CORES − 1` are dedicated `dsp_worker` threads (Cores 1..N−1); the coordinator on Core 0 is the N-th context and joins the same drain. The coordinator does **not** hand-assign work — it publishes the job array and bumps a generation counter; every context then **self-claims** the next job via `atomic_fetch_add` on a shared index. No per-core slot assignment, no load-balancing logic, no kernel wait on the hot path.

```c
typedef struct alignas(64) {
    pthread_t       tid;
    int             coreId;
    _Atomic bool    running;
    struct WorkerPool* pool;
    uint8_t pad[64 - (sizeof(pthread_t) + sizeof(int) + sizeof(_Atomic bool) + sizeof(void*))];
} DspWorker;

typedef struct WorkerPool {
    OptimizedAudioJob*  jobs;                 // job array for the current stage
    int                 jobCount;
    _Atomic int         nextJob;              // self-claim cursor (fetch_add)
    _Atomic int         jobsRemaining;        // barrier counter
    _Atomic uint32_t    generation;           // bumped on each dispatch → wakes workers
    AudioProcessingEngine* engine;
    DspWorker           worker[NUM_DSP_CORES - 1];   // dedicated workers, Cores 1..N-1
} WorkerPool;

// --- Shared drain: run by both the dedicated workers AND the coordinator ---
static void Pool_Drain(WorkerPool* p) {
    for (;;) {
        int idx = atomic_fetch_add_explicit(&p->nextJob, 1, memory_order_relaxed);
        if (idx >= p->jobCount) return;                    // nothing left to claim
        OptimizedAudioJob* j = &p->jobs[idx];
        if (!atomic_load_explicit(&p->engine->forceAbort, memory_order_relaxed))
            j->dspFunction(j->contextData, j->targetChannel, j->sampleCount);
        atomic_fetch_sub_explicit(&p->jobsRemaining, 1, memory_order_release);
    }
}

// --- Worker hot loop: HYBRID park/spin — low latency while busy, 0% CPU when idle ---
#define POOL_SPIN_LIMIT 1500     // ~tens of µs of spin before parking (tune on SoC)

static void* DspWorker_Main(void* arg) {
    DspWorker* w = (DspWorker*)arg;
    Thread_Pin(w->coreId, SCHED_FIFO, /*prio=*/89);
    uint32_t seen = 0;
    while (atomic_load_explicit(&w->running, memory_order_relaxed)) {
        uint32_t gen = atomic_load_explicit(&w->pool->generation, memory_order_acquire);
        if (gen == seen) {                                  // no new stage armed
            for (int s = 0; s < POOL_SPIN_LIMIT; ++s) {     // 1) spin briefly (stay hot, ~sub-µs wake)
                _mm_pause();
                gen = atomic_load_explicit(&w->pool->generation, memory_order_acquire);
                if (gen != seen) goto run;
            }
            futex_wait(&w->pool->generation, seen);         // 2) then PARK at 0% CPU until dispatch
            continue;
        }
    run:
        seen = gen;
        Pool_Drain(w->pool);                                // claim & run until array drained
    }
    return NULL;
}
```

Because claiming is a single `fetch_add`, the pool naturally balances any `jobCount` across the N cores: with 2 channels × 2 sub-bands and 4 cores it is one-per-core; with 8 jobs each core grabs ~2; with 1 job a single core runs it and the rest fall straight through. **Dispatch wakes parked workers:** after bumping `generation`, `Engine_RunParallel` issues a `futex_wake(&generation, NUM_DSP_CORES-1)`. The hybrid wake means active streaming pays only the spin cost (sub-µs latency) while idle blocks and `MODE_KEYWORD_LISTENING` (where the pool is never dispatched) let HP Cores 1–3 **park at 0% CPU and clock down** — the v2 power fix. Dispatch stays "publish, wake, and forget."

### 11.3 Thread Bring-Up (affinity + priority + paging)
Every thread is pinned and locked at init; after bring-up no thread is ever created or destroyed on the audio path.

```c
void Thread_Pin(int coreId, int policy, int priority) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(coreId, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);   // hard affinity, no migration
    struct sched_param sp = { .sched_priority = priority };
    pthread_setschedparam(pthread_self(), policy, &sp);          // SCHED_FIFO RT band
}
// Once, globally: mlockall(MCL_CURRENT | MCL_FUTURE)  → static pools never paged out.
```

### 11.4 Synchronization Primitive Policy
The entire cross-thread surface is built from four lock-free mechanisms — **all blocking kernel primitives are banned on RT threads**:

| Need | Mechanism | Direction |
|------|-----------|-----------|
| Control state (mode, drift, volume, error) | `_Atomic` fields in `SharedFrameworkContext` | LP 4 → HP 0 |
| Parallel job dispatch + join | shared job array + `nextJob` claim index + `jobsRemaining` | HP 0 ↔ HP 1–3 |
| Audio sample handoff across a boundary | SPSC lock-free ring buffer | producer → consumer |
| Async triggers / long-task offload | bounded lock-free **MPSC priority-lane dispatcher** (§13) | any core → 1 ms LP / RT edge / bkgd |

**Banned on Cores 0–4:** `pthread_mutex`, `pthread_cond`, `sem_wait`, `malloc`/`free`, `printf`, blocking syscalls — any of which can incur an unbounded scheduler wait.

### 11.5 Memory Model & Correctness
* **Release/acquire pairing:** dispatch does a `release` bump of `generation`; the worker's `acquire` load makes the published job array (and the mic/reference buffers it reads) visible without a lock. Claiming is a `relaxed` `fetch_add` on `nextJob` — mutual exclusion comes from the *uniqueness* of the returned index, not from ordering. The `jobsRemaining` decrement is `release` and the coordinator's barrier load is `acquire`, establishing happens-before on the AEC results before Beamforming reads them.
* **False sharing:** every concurrently-written object (`DspWorker`, `OptimizedAudioJob`, `SharedFrameworkContext`, `AudioEventMsg`) is `alignas(64)` so no two cores fight over a cache line (§1.1).
* **Power (hybrid park/spin, §11.2):** workers spin only for `POOL_SPIN_LIMIT` iterations after going idle, then **futex-park at 0% CPU** until the next dispatch `futex_wake`. Combined with mode-adaptive graphs (§10.4) — where `MODE_KEYWORD_LISTENING` never dispatches the pool — HP Cores 1–3 stay parked and clock down through the device's most common state, while active streaming still gets sub-µs wake latency. This is what makes the big.LITTLE choice pay off instead of burning the big cluster on `_mm_pause()`.

### 11.6 Execution Context Model — Per-Core Threads, Per-Node Data (NOT per-node threads)
A node is **not** a thread. With an open-ended, unknown node count, a thread-per-node design would oversubscribe the 4 big cores and reintroduce the kernel scheduler, context-switching, and unbounded jitter — defeating §11 entirely. The framework instead separates three contexts:

| Context | Scope | What it holds | Lifetime |
|---------|-------|---------------|----------|
| **Thread / execution context** | **per core** (fixed = `NUM_DSP_CORES` + LP threads) | pinned `SCHED_FIFO` run loop (`PipelineCore_Main`) | created once at boot; never on the RT path |
| **Node context** | **per node instance** | state struct + `inQ` + `outQ` — pure **data** | static, built at init (§12) |
| **Task / Job** | **per parallel sub-slice** | transient `OptimizedAudioJob` (run-to-completion) | one dispatch |

* **One thread per core, many nodes per thread.** A core's thread runs *every* node whose `coreId` matches it, in pipeline order, triggered by queue data availability. Nodes are decoupled by **SPSC queues, not by separate threads**.
* **Scaling is free of threads.** Adding 50 nodes adds 50 data instances spread across the same fixed core-threads — zero new threads, zero new scheduling pressure. Thread count is bound to *cores*, never to *nodes*.
* **Heavy nodes borrow, never own.** A compute-heavy node (AEC) parallelizes by transiently borrowing all core-threads via the self-claim pool (§10.7), then returns them to pipeline mode. It never holds a dedicated thread.
* **LP cluster mirrors this:** the 1 ms scheduler (LP 4) and background workers (LP 5–7) are likewise per-core threads; "long tasks" are *data* on the `AudioEventBus`, not per-task threads.

This "thread-per-core, node-as-data" (staged run-to-completion) model is what keeps the system bounded and jitter-free as the node set grows without limit.

---

## 12. Scalable Node Framework (Open-Ended Node Set)
The node set is **unbounded and unknown at design time** — new functionality (noise suppression, dereverb, AGC, an extra detector, a test tap) is added continuously. The framework therefore treats nodes as **plugins**: a new node type is *registered and wired by data*, and the engine, queues, dispatcher, and recovery code (§10–§11) **never change**. This section specifies how an arbitrary number of node *types* and *instances* scale without code churn or heap.

### 12.1 The Extension Contract (everything a new node must supply — and nothing more)
To add a brand-new capability you implement exactly one unit and touch **no engine code**:
1. a **state struct** (the node's private "member variables");
2. the four `NodeVTable` methods (`prepare`, `reset`, `process`, `fillJob`) — §4.0;
3. one `static const NodeVTable`;
4. one **self-registration** line.

Everything else — queue allocation, core placement, pipelining, fan-out, watchdogs, overflow recovery — is provided by the framework and is identical for every node.

### 12.2 Self-Registering Node-Type Registry
Node *types* live in a registry populated by **self-registration**, so dropping a new `.c` file into the build adds a type with **zero edits to any central list**. Each module registers itself via a constructor; the engine discovers types by name at graph-build time.

```c
typedef struct {
    const char*       typeName;     // "src", "aec", "noise_suppress", "agc", …  (unique key)
    const NodeVTable* vt;           // the type's methods (§4.0)
    size_t            stateSize;    // bytes of per-instance state to carve from the arena
    void (*configure)(ProcessingNode* n, const NodeConfig* cfg);   // optional param init
} NodeType;

void NodeRegistry_Add(const NodeType* t);                 // called at startup, init-time only
const NodeType* NodeRegistry_Find(const char* typeName);  // resolved during graph build

// A node module registers itself — no engine file is edited when a new type appears:
#define REGISTER_NODE_TYPE(symbol) \
    __attribute__((constructor)) static void _reg_##symbol(void){ NodeRegistry_Add(&symbol); }

// e.g. in noise_suppress.c:
static const NodeType NS_TYPE = { "noise_suppress", &NS_VTABLE, sizeof(NsState), Ns_Configure };
REGISTER_NODE_TYPE(NS_TYPE);
```

### 12.3 Zero-Heap Arena — capacity is a single knob
Because instance count is unknown, node state, input queues, and payload slots are bump-allocated **once at init** from a single statically-reserved **arena** (never `malloc`; never on the RT path). Scaling the deployment up is changing *one number*; a `_Static_assert`/runtime guard catches overflow at boot, not in the field.

```c
#define NODE_ARENA_BYTES (256 * 1024)     // ONE tunable — size for the deployed graph
alignas(64) static uint8_t gArena[NODE_ARENA_BYTES];
static size_t gArenaUsed;

void* Arena_Alloc(size_t n) {             // init-time bump allocator (RT path never calls this)
    n = (n + 63) & ~63u;                  // 64-byte align → no false sharing between instances
    if (gArenaUsed + n > NODE_ARENA_BYTES) Fatal("node arena exhausted — raise NODE_ARENA_BYTES");
    void* p = &gArena[gArenaUsed]; gArenaUsed += n; return p;
}
```
Sizing rule (documented for integrators): `NODE_ARENA_BYTES ≥ Σ stateSize(instances) + N·sizeof(SpscPointerQueue) + poolSlots·sizeof(AudioBufferPayload)`. `MAX_NODES` (§3) is just the instance-handle count; raise both together.

### 12.4 Data-Driven Graph Construction (adding a node = one config row)
The concrete pipeline — *which* node instances exist, their parameters, core placement, and wiring, per `EngineMode` — is **declarative data**, parsed once at init by a generic builder. This is the payoff: the node set changes by editing a table (or an external config file / device-tree / XML), never the engine.

```c
typedef struct {
    const char* type;       // registry key (§12.2)
    const char* instance;   // unique instance name (wiring target)
    int         coreId;     // pipeline placement (§10.4)
    const char* feedsInto;  // downstream instance name, or NULL at the sink
    NodeConfig  cfg;         // type-specific params (taps, thresholds, …)
} NodeDecl;

// The ONLY thing that changes when the node set changes — pure data:
static const NodeDecl kStreamingGraph[] = {
  { "alsa_capture",   "cap",  0, "src"  , {0} },
  { "src",            "src",  0, "aec"  , {0} },
  { "aec",            "aec",  1, "beam" , {0} },
  { "noise_suppress", "ns",   1, "beam" , {0} },   // ← inserted later: ZERO engine changes
  { "beamform",       "beam", 2, "perc" , {0} },
  { "perception",     "perc", 3, "out"  , {0} },
  { "output",         "out",  0, NULL   , {0} },
};

// Generic builder: instantiate from registry into the arena, then resolve wiring by name.
void Engine_BuildFromDecl(AudioEngine* eng, const NodeDecl* decl, int count, EngineMode mode) {
    for (int i = 0; i < count; ++i) {                         // pass 1: instantiate
        const NodeType* t = NodeRegistry_Find(decl[i].type);
        ProcessingNode* n = &eng->nodes[eng->nodeCount++];
        n->vt = t->vt; n->state = Arena_Alloc(t->stateSize); n->coreId = decl[i].coreId;
        if (t->configure) t->configure(n, &decl[i].cfg);
        n->vt->prepare(n, 48000, DSP_BLOCK_SAMPLES);
    }
    for (int i = 0; i < count; ++i)                           // pass 2: wire outQ by instance name
        if (decl[i].feedsInto)
            FindNode(eng, decl[i].instance)->outQ = &FindNode(eng, decl[i].feedsInto)->inQ;
    eng->graph[mode].sink = FindNode(eng, /*the NULL-feedsInto node*/);
}
```
Adding the noise-suppress capability above was: write `noise_suppress.c` (§12.1) + one table row. No edit to `Node_RunOnce`, the dispatcher, the pool, or recovery.

### 12.5 Scaling Bounds & Placement Guidance
The framework scales nodes freely, but two physical budgets bound a *deployment* and must be checked at build time:
* **Per-core time budget:** the sum of stage costs placed on one `coreId` must fit the 5 ms block. Many light nodes share a core; a heavy node (AEC) gets its own and borrows the pool for intra-node bursts (§10.4).
* **Pipeline depth ↔ latency:** each *serial* stage adds ~5 ms end-to-end (§10.8). Prefer **parallel branches** (independent nodes on sibling queues that a later node merges) over one long chain when latency matters; depth, not node *count*, is the latency driver.
* **Arena & handles:** `NODE_ARENA_BYTES` and `MAX_NODES` are the only ceilings; both are single knobs with boot-time guards.

### 12.6 Optional Hot Reconfiguration (init-time by default)
Graphs are built once at init. If a deployment needs to add/remove a node *at runtime*, build the new graph in a shadow arena and swap `eng->graph[mode].sink` with a single atomic store at a block boundary, after draining in-flight buffers. This is opt-in; the default and RT-safe path is static construction at boot.

---

## 13. Asynchronous Trigger Subsystem (Priority-Based)
A main LLM-interactive device generates **frequent, asynchronous triggers** of very different urgency — constant barge-in, TTS start/stop, streaming far-end chunks, mode changes, wake events, hardware events. The single-slot event bus of v1 would drop triggers under burst. v2 replaces it with a **priority-based trigger dispatcher**: bounded lock-free MPSC lanes, drained highest-priority-first, with each class routed to the consumer and timescale that fits it.

**Priority is delivered by structure, not by preempting the audio path.** A 5 ms DSP block is <1 ms run-to-completion on one A76 and is never preempted; urgent work runs **concurrently on the LP cluster**, so there is **no preemptive priority job-scheduler** — only a priority-aware *dispatcher*.

### 13.1 Trigger Classes
| Class | Examples | Consumer | Latency | Lane |
|-------|----------|----------|---------|------|
| **Urgent** | barge-in duck, hard mute, mic/HW unplug | 1 ms LP scheduler (acts between blocks) | ≤ 1 ms | `PRIO_URGENT` |
| **Block-aligned** | mode/graph swap, far-end TTS chunk, volume, config | RT sample core (drains at the 5 ms block edge) | ≤ 5 ms | `PRIO_NORMAL` |
| **Deferred** | STT upload, cloud notify, logging, metrics | LP background workers | best-effort | `PRIO_DEFERRED` |

### 13.2 Priority Trigger Dispatcher (the "high-priority dispatcher")
One bounded lock-free **MPSC ring per priority lane**; any core posts (priority-tagged); each consumer drains the **highest non-empty lane first**. This is the whole "priority dispatcher" — no preemptive scheduling, no runtime job sort.

```c
typedef enum { PRIO_URGENT = 0, PRIO_NORMAL = 1, PRIO_DEFERRED = 2, PRIO_LANES } TriggerPrio;

typedef struct alignas(64) {                 // bounded MPSC ring (multi-producer, one consumer)
    AudioEventMsg    ring[TRIGGER_RING_CAPACITY];
    _Atomic uint32_t head;                    // producers CAS-advance
    _Atomic uint32_t tail;                    // single consumer
} TriggerQueue;

typedef struct { TriggerQueue lane[PRIO_LANES]; } PriorityDispatcher;

// Producer: never blocks; overflow escalates instead of stalling the caller.
static inline void Trigger_Post(PriorityDispatcher* d, TriggerPrio p, const AudioEventMsg* m) {
    if (!Trigger_Push(&d->lane[p], m)) Trigger_OnOverflow(d, p, m);   // §13.5
}

// Consumer: strict priority — drain URGENT, then NORMAL, then DEFERRED.
static bool Trigger_DrainNext(PriorityDispatcher* d, AudioEventMsg* out) {
    for (int p = 0; p < PRIO_LANES; ++p)
        if (Trigger_Pop(&d->lane[p], out)) return true;
    return false;
}
```

### 13.3 Three Consumers, Three Timescales
* **Urgent → 1 ms LP scheduler.** Between 5 ms audio blocks, the LP scheduler drains the urgent lane and acts immediately — e.g. starts the dB fader (barge-in duck) or hard-mutes. Runs on the A55 cluster *concurrently* with the A76 DSP block, so it needs no preemption of the audio path.
* **Block-aligned → RT sample core.** At each block edge, *before* processing audio, the engine drains pending normal triggers and applies them **glitch-free** (mode swap, install new far-end chunk, set volume). Bounded drain → part of WCET.
* **Deferred → LP background workers.** Drained off the RT path entirely; network/file/cloud latency never touches the audio cadence.

```c
void Engine_Block(AudioEngine* eng, AudioBufferPayload* in, AudioBufferPayload* out) {
    AudioEventMsg t;                                   // (1) apply async state at the block edge
    for (int n = 0; n < TRIGGER_DRAIN_MAX && Trigger_Pop(&eng->normal, &t); ++n)
        Engine_ApplyTrigger(eng, &t);                  //     mode/ref/volume — atomic, glitch-free
    Pipeline_RunSequential(eng, eng->graph[eng->mode].sink, in, out);   // (2) bounded audio work
}
```

### 13.4 Priority Enforcement — three layers, no preemptive scheduler
1. **Priority lanes** (this section): urgent triggers are drained before all others.
2. **Timescale separation:** urgent work lives on the 1 ms plane (5× finer than the audio block), so it acts before the next block even begins.
3. **`SCHED_FIFO` bands** (§11.1): the urgent handler outranks background workers on the LP cluster, so the kernel preempts best-effort work for it. Within the A76 cluster the DSP block is never preempted — urgency is always on the *other* cluster.

The barge-in hot path end-to-end: `VAD detect → Trigger_Post(URGENT, BARGE_IN) → 1 ms LP duck (≤1 ms, silence in ~12 ms) → Trigger_Post(NORMAL, TTS_STOP) → RT block-edge mode swap (≤5 ms) → Trigger_Post(DEFERRED, halt-cloud-TTS) → background`.

### 13.5 Coalescing & Overflow (handling frequent bursts)
* **Coalesce idempotent triggers:** a flood of `SET_VOLUME` / `SET_MODE` collapses to the **latest** in-lane → O(1) at drain, no backlog.
* **Reserved urgent lane:** the urgent lane is sized and separate so a storm of deferred/normal triggers can **never starve or delay** a barge-in.
* **Overflow escalates, never blocks:** if a sized lane fills, `Trigger_OnOverflow` sets `lastError` and signals the 1 ms scheduler (the §10.5 recovery path) — the posting producer is never blocked.

### 13.6 Why this fits the sequential engine perfectly
The inbound trigger stream has **exactly one consumer** (the sequential RT core), so applying a trigger to engine state is **race-free and ordered by construction** — no lock, no cross-core agreement on "which mode are we in." A parallel/pipelined engine would have made consistent state application genuinely hard; the sequential design makes frequent async triggers trivial: **post asynchronously, drain by priority, apply at a deterministic edge.**

---

## 14. Deployment Architecture — PipeWire Data Plane + MsgBus Control Plane

The Hermes deployment runs the §1–§13 engine inside a **two-plane architecture**: **PipeWire** owns the **data plane** (move and clock PCM), and a **TradingAlpha-style MsgBus** owns the **control plane** (define, route, and dispatch every command and event). The two planes never overlap — *samples* never travel on the MsgBus, and *decisions* never travel through PipeWire buffers. This section specifies the plane split, the Hermes module/process topology, the `CMsg` header and `EventMap` dispatch the project uses, the full command/event catalog, and the **symmetric RT boundary** that keeps the MsgBus off the audio hot path.

> **Relationship to §1–§13.** The DSP engine specified in §1–§13 is **not replaced**; it is hosted as a **single PipeWire SPA node** (the "RT island", §14.4). The §1.1 core principles (zero-alloc, lock-free, cache-aligned, two-timescale) are preserved *inside* that node; PipeWire provides the clock, device I/O, and routing *around* it. §14.4 lists exactly what changes versus the standalone assumptions of §2.1/§5/§10.6.

### 14.1 Plane Separation Principle
**If it is samples → PipeWire. If it is a decision → MsgBus.** The two planes have opposite requirements and are engineered separately:

| | **Data plane → PipeWire** | **Control plane → MsgBus** |
|---|---|---|
| Carries | mic PCM, processed mono, AEC reference, playback, per-buffer timestamps | cmd/event `CMsg` structs (catalog §14.6) |
| Rate | continuous, every 5 ms quantum | discrete, event-driven, bursty |
| Latency | hard-RT, zero-copy shared memory | µs–ms, non-RT |
| Transport | PipeWire graph (shm buffers) | SysV mq + shared-memory (cross-process) |
| Dispatch | the §10 engine / SPA node | `EventMap` (id → handler), §14.7 |
| Threads | PipeWire data-loop (RT) | MsgBus recv threads + forwarder (non-RT, LP 5–7) |
| Clock | PipeWire driver (`spa_io_position`) | rides the data-plane clock via `evt::CLOCK_ANCHOR` |

### 14.2 Process & Module Topology
Each subsystem is its own process and is **both** a PipeWire client (data) **and** a MsgBus module (one `ModuleId`, one `EventMap`). Processes needing *samples* (voice-trigger, STT, video) are PipeWire clients receiving zero-copy links — no audio crosses the MsgBus.

```
            ┌──────────────────── CONTROL PLANE (MsgBus: SysV mq + shm, non-RT) ───────────────────┐
 SUPERVISOR(1) ◄─► AUDIO_CORE(2) ◄─► VOICE_TRIGGER(3) ◄─► VIDEO_PROC(4) ◄─► STT_CLOUD(5) ◄─► CODEC_HW(6)
            └───────────────────────────────────┬──────────────────────────────────────────────────┘
 ═══════════════════════════════════════════════╪══════════════════════════════════════════════════════
            ┌──────────────────── DATA PLANE (PipeWire graph, zero-copy shm, RT) ────────────────────┐
   [ALSA mic] ─┐                                                                                      │
               ├─► [AUDIO_CORE DSP node] ─► clean mono ─┬─► [ALSA speaker sink]                       │
 [playback]─►[sink monitor = AEC ref] ─┘                └─(PipeWire links)─► VOICE_TRIGGER · STT · VIDEO
            └────────────────────────────────────────────────────────────────────────────────────────┘
```

### 14.3 Data Plane — PipeWire Graph
* **One driver clock.** The ALSA device is the graph **driver**; quantum is **locked** to 240 samples / 5 ms (§14.11). Every node runs once per quantum; the whole graph completes within the 5 ms cycle (one-block latency — *not* depth×5 ms; this supersedes the async-pipeline latency caveat of §10.8 for the single-node deployment).
* **Custom AEC retained.** The DSP node consumes **two** inputs — mic capture and the playback **sink monitor** (the far-end reference, tapped **post-fader**, §4.3.2e) — and runs the §4.3 PBFDAF internally. (Alternative: PipeWire `module-echo-cancel`/WebRTC AEC3, which trades the custom PBFDAF + DTD for built-in delay/double-talk handling. Hermes keeps the custom AEC.)
* **Master timestamp.** `spa_io_position.clock.{nsec,position,rate}` replaces the §3 `hardwareTimestampNs` as the timeline source, and is the value published in `evt::CLOCK_ANCHOR` for A/V sync (§14.10).
* **Routing = WirePlumber policy** expresses the per-`EngineMode` graph wiring (§4 mode graphs).

### 14.4 The RT Island — §1–§13 Engine as One SPA Node
The entire §1–§13 engine becomes the `process()` of **one** PipeWire filter node. Inside that callback the §1.1 principles hold unchanged; only the *machine-ownership* assumptions move to PipeWire:

| §1–§13 assumption | Under PipeWire (single-node island) |
|---|---|
| §2.1 Core 0 woken by **ALSA DMA IRQ** | Woken by PipeWire's data-loop (timer-based by default; IRQ-like with `period-size=quantum`, `headroom=0`). The wake is PipeWire's, not a bare IRQ handler. |
| §10.6 **ALSA mmap** onto static pool (engine owns device) | PipeWire owns the device and hands the node zero-copy buffers; capture stays copy-free, but PipeWire owns it. |
| §5 clock-drift **PI loop + ASRC** | PipeWire's DLL/adaptive-resampler does cross-device drift; the §5 loop is dropped (or kept only for an in-island sub-rate). |
| §6.1 per-block **forceAbort + soft-mute** | Re-implemented **inside** the node: compare `now_ns() − clock.nsec` to the quantum budget → zero-fill (§14.11). PipeWire's own miss = plain xrun. |
| §6.2 DMA-interval watchdog | PipeWire surfaces xrun/error via main-loop callbacks → mapped to `evt::XRUN` (§14.9). |
| §10/§11 inter-stage SPSC + worker pool | **Preserved inside the node** (one node = no PipeWire inter-node scheduling). The self-claim pool (§11.2) runs as the node's own threads on the big cores. |
| §3 zero-alloc static pools, §11 lock-free, alignas(64) | **Preserved** — this is the node's own code. |

**Net:** principles that are *about the node's code* survive verbatim; principles that are *whole-machine RT guarantees* (exact-IRQ wake, device ownership, hard abort) are relocated into the node or downgraded to PipeWire's soft-RT model. This is the deliberate trade for PipeWire's routing/duplex/clock/multi-process benefits.

### 14.5 Control Plane — The `CMsg` Header (Hermes)
The control plane is a **C++ MsgBus** (non-RT; the RT island stays C11). Every command and event is a `CMsg`: a fixed POD header + an **inline** body (tiny POD, carried in the mq message). The header is the project-wide wire contract.

```c
#define HERMES_PROTOCOL_VERSION 1

#pragma pack(4)
typedef struct {                 // 20-byte fixed control header (wire-stable)
    uint16_t version;            // HERMES_PROTOCOL_VERSION
    uint16_t id;                 // catalog key (§14.6) — encodes owner module + cmd/evt
    uint8_t  src;                // ModuleId of sender
    uint8_t  dest;               // ModuleId of recipient
    uint8_t  prio;               // maps to §13 TriggerPrio lane (URGENT/NORMAL/DEFERRED)
    uint8_t  flags;              // reserved
    uint32_t length;             // inline body byte length (0 if none)
    uint64_t timestampNs;        // PipeWire clock domain (spa_io_position.clock.nsec)
} CMsgHead;
#pragma pack()

typedef struct {
    CMsgHead hdr;
    void*    pBody;              // inline body bytes (carried in the mq message)
} CMsg;
```

* **`prio`** routes a cross-process message onto the same three lanes as the in-process §13 dispatcher, so urgency is preserved end-to-end (a barge-in `CMsg` cannot queue behind a metrics upload).
* **`timestampNs`** is stamped from the PipeWire clock so events are correlatable across processes (A/V sync, log alignment).
* **Bodies are inline POD** carried in the mq message — no alloc-type tag, no cross-process pointers. Bulk audio never uses the control plane (it goes via the `PrerollRing`/PipeWire, §16); a `QUERY_STATE` reply returns as an ordinary async event, so there is no blocking request/reply field.

### 14.5.1 Module IDs & the Command/Event Numbering Scheme
Each module owns a **256-ID block** (`ModuleId × 0x100`), split into **commands** (low 128 — imperatives *to* the module) and **events** (high 128 — notifications *from* the module). From an `id` alone you know the owner and whether it is a command or an event — collision-free and self-describing.

```c
namespace hermes { namespace ModuleId {
enum {
    SUPERVISOR    = 1,   // lifecycle, mode policy, health
    AUDIO_CORE    = 2,   // PipeWire host + §1–§13 DSP RT island
    VOICE_TRIGGER = 3,   // ALWAYS-ON keyword detection (full §7 KWD) — own process (§16)
    VIDEO_PROC    = 4,   // video processing + A/V sync
    CLOUD_CONNECTOR = 5, // on-target proxy: PipeWire client ↔ network (STT/LLM/TTS); PipeWire never reaches cloud
    CODEC_HW      = 6,   // I2C codec / hardware control
};
}} // namespace hermes::ModuleId

#define HM_CMD_EVT_GAP   0x80
#define HM_CMD_FIRST(m)  ((m) * 0x0100)              // commands: base .. base+0x7F
#define HM_EVT_FIRST(m)  (HM_CMD_FIRST(m) + HM_CMD_EVT_GAP)  // events:  base+0x80 ..
```

### 14.6 Hermes Command & Event Catalog
The complete control surface. IDs are stable; append within a module's block, never renumber (reserve removed IDs, as TradingAlpha does).

```c
namespace hermes {
using namespace ModuleId;

// ════════════════════════ SUPERVISOR (1) — orchestrator / Session FSM (§15) ════════════════════════
namespace _Supervisor {
namespace cmd {                                          // external/UI control of the session
  static const uint16_t SHUTDOWN         = HM_CMD_FIRST(SUPERVISOR) + 0;  // graceful teardown
  static const uint16_t SET_MODE_POLICY  = HM_CMD_FIRST(SUPERVISOR) + 1;  // e.g. push-to-talk vs always-listen
  static const uint16_t START_SESSION    = HM_CMD_FIRST(SUPERVISOR) + 2;  // force a turn (button / PTT) — skip wake
  static const uint16_t CANCEL_SESSION   = HM_CMD_FIRST(SUPERVISOR) + 3;  // abort current turn → IDLE
  static const uint16_t FACTORY_RESET    = HM_CMD_FIRST(SUPERVISOR) + 4;
  static const uint16_t QUERY_STATE      = HM_CMD_FIRST(SUPERVISOR) + 5;  // sync → current SessionState
  static const uint16_t LAST             = HM_CMD_FIRST(SUPERVISOR) + 5;
}
namespace evt {                                          // FSM lifecycle broadcasts
  static const uint16_t STATE_CHANGED    = HM_EVT_FIRST(SUPERVISOR) + 0;  // body: {from, to, SessionState} (§15)
  static const uint16_t SESSION_STARTED  = HM_EVT_FIRST(SUPERVISOR) + 1;
  static const uint16_t SESSION_ENDED    = HM_EVT_FIRST(SUPERVISOR) + 2;  // body: end-reason
  static const uint16_t FAULT            = HM_EVT_FIRST(SUPERVISOR) + 3;  // aggregated, body: HardwareError
  static const uint16_t READY            = HM_EVT_FIRST(SUPERVISOR) + 4;  // boot complete → IDLE
  static const uint16_t LAST             = HM_EVT_FIRST(SUPERVISOR) + 4;
}
namespace internal {                                     // FSM-private timer events (not on the wire)
  static const uint16_t TO_WAKE_CONFIRM  = HM_EVT_FIRST(SUPERVISOR) + 16; // wake-confirm window expired
  static const uint16_t TO_NO_SPEECH     = HM_EVT_FIRST(SUPERVISOR) + 17; // capture: no speech in window
  static const uint16_t TO_RESPONSE      = HM_EVT_FIRST(SUPERVISOR) + 18; // think: cloud too slow
  static const uint16_t TO_SESSION_MAX   = HM_EVT_FIRST(SUPERVISOR) + 19; // overall turn cap
  static const uint16_t TO_RESET         = HM_EVT_FIRST(SUPERVISOR) + 20; // fault recovery watchdog
}}

// ════════════════════════ AUDIO_CORE (2) — §1–§13 DSP RT island ════════════════════════
namespace _AudioCore {
namespace cmd {                                          // imperatives the engine obeys (→ atomics, §14.8)
  static const uint16_t SET_MODE        = HM_CMD_FIRST(AUDIO_CORE) + 0;  // EngineMode (§3) — body: EngineMode
  static const uint16_t START_CAPTURE   = HM_CMD_FIRST(AUDIO_CORE) + 1;  // route clean mono → STT (data plane)
  static const uint16_t STOP_CAPTURE    = HM_CMD_FIRST(AUDIO_CORE) + 2;
  static const uint16_t PLAY_TTS        = HM_CMD_FIRST(AUDIO_CORE) + 3;  // open playback path (far-end active)
  static const uint16_t STOP_TTS        = HM_CMD_FIRST(AUDIO_CORE) + 4;  // halt playback, flush far-end
  static const uint16_t DUCK_PLAYBACK   = HM_CMD_FIRST(AUDIO_CORE) + 5;  // barge-in duck — body: {target_dB, ramp_ms}
  static const uint16_t SET_VOLUME      = HM_CMD_FIRST(AUDIO_CORE) + 6;  // dB fader (§5.1.3)
  static const uint16_t FREEZE_ADAPT    = HM_CMD_FIRST(AUDIO_CORE) + 7;  // DTD freeze AEC weights (§4.3) — body: bool
  static const uint16_t ARM_BARGE_IN    = HM_CMD_FIRST(AUDIO_CORE) + 8;  // enable barge-in VAD gating (§8)
  static const uint16_t DISARM_BARGE_IN = HM_CMD_FIRST(AUDIO_CORE) + 9;
  static const uint16_t RESET_PIPELINE  = HM_CMD_FIRST(AUDIO_CORE) + 10; // §6.2 purge + recalibrate
  static const uint16_t REF_RELOCK      = HM_CMD_FIRST(AUDIO_CORE) + 11; // re-run x-corr delay lock (§4.3.2c)
  static const uint16_t QUERY_STATE     = HM_CMD_FIRST(AUDIO_CORE) + 12; // sync → mode/ERLE/xrun counters
  static const uint16_t LAST            = HM_CMD_FIRST(AUDIO_CORE) + 12;
}
namespace evt {                                          // RT-island notifications (→ ring → forwarder, §14.8)
  // +0 RESERVED — KWD_CANDIDATE retired; keyword detection lives ENTIRELY in VTS (§16.1)
  static const uint16_t BARGE_IN        = HM_EVT_FIRST(AUDIO_CORE) + 1;  // §8 — KEY PATH, RT island, URGENT lane (§13)
  static const uint16_t VAD_SPEECH_ON   = HM_EVT_FIRST(AUDIO_CORE) + 2;
  static const uint16_t VAD_SPEECH_OFF  = HM_EVT_FIRST(AUDIO_CORE) + 3;  // dwell-confirmed end of utterance
  static const uint16_t CAPTURE_STARTED = HM_EVT_FIRST(AUDIO_CORE) + 4;
  static const uint16_t PLAYBACK_STARTED= HM_EVT_FIRST(AUDIO_CORE) + 5;
  static const uint16_t PLAYBACK_DRAINED= HM_EVT_FIRST(AUDIO_CORE) + 6;  // far-end emptied → TTS audibly done
  static const uint16_t REF_LOCKED      = HM_EVT_FIRST(AUDIO_CORE) + 7;  // §4.3.2 delay lock acquired
  static const uint16_t AEC_ERLE_DROP   = HM_EVT_FIRST(AUDIO_CORE) + 8;  // divergence → re-lock trigger
  static const uint16_t SOFT_MUTE       = HM_EVT_FIRST(AUDIO_CORE) + 9;  // §6.1 deadline fallback fired
  static const uint16_t XRUN            = HM_EVT_FIRST(AUDIO_CORE) + 10; // §10.5 / PipeWire xrun
  static const uint16_t MODE_CHANGED    = HM_EVT_FIRST(AUDIO_CORE) + 11; // ack of SET_MODE / RESET completion
  static const uint16_t CLOCK_ANCHOR    = HM_EVT_FIRST(AUDIO_CORE) + 12; // {clock.nsec, sample_pos, rate} → A/V sync
  static const uint16_t LAST            = HM_EVT_FIRST(AUDIO_CORE) + 12;
}}

// ════════════════════════ VOICE_TRIGGER (3) — wake confirmation ════════════════════════
namespace _VoiceTrigger {
namespace cmd {
  static const uint16_t ARM           = HM_CMD_FIRST(VOICE_TRIGGER) + 0;  // enable scoring (IDLE entry)
  static const uint16_t DISARM        = HM_CMD_FIRST(VOICE_TRIGGER) + 1;  // suppress during a turn
  static const uint16_t SET_THRESHOLD = HM_CMD_FIRST(VOICE_TRIGGER) + 2;
  static const uint16_t LAST          = HM_CMD_FIRST(VOICE_TRIGGER) + 2;
}
namespace evt {
  static const uint16_t WAKE_CONFIRMED = HM_EVT_FIRST(VOICE_TRIGGER) + 0; // post-scored wake → start turn
  static const uint16_t WAKE_REJECTED  = HM_EVT_FIRST(VOICE_TRIGGER) + 1; // candidate failed 2nd-stage
  static const uint16_t LAST           = HM_EVT_FIRST(VOICE_TRIGGER) + 1;
}}

// ════════════════════════ VIDEO_PROC (4) — A/V sync ════════════════════════
namespace _VideoProc {
namespace cmd {
  static const uint16_t SYNC_ANCHOR = HM_CMD_FIRST(VIDEO_PROC) + 0;  // push audio PTS↔wallclock map
  static const uint16_t START       = HM_CMD_FIRST(VIDEO_PROC) + 1;
  static const uint16_t STOP        = HM_CMD_FIRST(VIDEO_PROC) + 2;
  static const uint16_t LAST        = HM_CMD_FIRST(VIDEO_PROC) + 2;
}
namespace evt {
  static const uint16_t SYNC_DRIFT  = HM_EVT_FIRST(VIDEO_PROC) + 0;  // A/V offset exceeded tolerance
  static const uint16_t FRAME_DROP  = HM_EVT_FIRST(VIDEO_PROC) + 1;
  static const uint16_t LAST        = HM_EVT_FIRST(VIDEO_PROC) + 1;
}}

// ════════════════ CLOUD_CONNECTOR (5) — on-target proxy: PipeWire client ↔ network (STT⊕LLM⊕TTS) ════════════════
namespace _Cloud {
namespace cmd {
  static const uint16_t OPEN_STREAM   = HM_CMD_FIRST(CLOUD_CONNECTOR) + 0;  // begin upstream STT for this turn
  static const uint16_t CLOSE_STREAM  = HM_CMD_FIRST(CLOUD_CONNECTOR) + 1;
  static const uint16_t UTTERANCE_END = HM_CMD_FIRST(CLOUD_CONNECTOR) + 2;  // endpoint hint (commit to LLM)
  static const uint16_t ABORT         = HM_CMD_FIRST(CLOUD_CONNECTOR) + 3;  // barge-in: cancel in-flight LLM/TTS
  static const uint16_t LAST          = HM_CMD_FIRST(CLOUD_CONNECTOR) + 3;
}
namespace evt {
  static const uint16_t CONNECTED       = HM_EVT_FIRST(CLOUD_CONNECTOR) + 0;
  static const uint16_t DISCONNECTED    = HM_EVT_FIRST(CLOUD_CONNECTOR) + 1;  // network lost
  static const uint16_t STT_PARTIAL     = HM_EVT_FIRST(CLOUD_CONNECTOR) + 2;
  static const uint16_t STT_FINAL       = HM_EVT_FIRST(CLOUD_CONNECTOR) + 3;
  static const uint16_t STT_ENDPOINT    = HM_EVT_FIRST(CLOUD_CONNECTOR) + 4;  // server VAD says utterance complete
  static const uint16_t STT_NO_SPEECH   = HM_EVT_FIRST(CLOUD_CONNECTOR) + 5;
  static const uint16_t LLM_BEGIN       = HM_EVT_FIRST(CLOUD_CONNECTOR) + 6;  // response generation started
  static const uint16_t TTS_CHUNK       = HM_EVT_FIRST(CLOUD_CONNECTOR) + 7;  // audio chunk ready (network → PipeWire)
  static const uint16_t TTS_STREAM_END  = HM_EVT_FIRST(CLOUD_CONNECTOR) + 8;  // last chunk delivered
  static const uint16_t CLOUD_ERROR     = HM_EVT_FIRST(CLOUD_CONNECTOR) + 9;
  static const uint16_t LAST            = HM_EVT_FIRST(CLOUD_CONNECTOR) + 9;
}}

// ════════════════════════ CODEC_HW (6) — hardware / I2C / buttons ════════════════════════
namespace _CodecHw {
namespace cmd {
  static const uint16_t RESET    = HM_CMD_FIRST(CODEC_HW) + 0;   // I2C codec reset (§5.2 long task)
  static const uint16_t SET_GAIN = HM_CMD_FIRST(CODEC_HW) + 1;
  static const uint16_t MUTE     = HM_CMD_FIRST(CODEC_HW) + 2;   // hard mute (privacy)
  static const uint16_t UNMUTE   = HM_CMD_FIRST(CODEC_HW) + 3;
  static const uint16_t LAST     = HM_CMD_FIRST(CODEC_HW) + 3;
}
namespace evt {
  static const uint16_t UNPLUGGED   = HM_EVT_FIRST(CODEC_HW) + 0;  // §6.2 / pw registry global_remove
  static const uint16_t PLUGGED     = HM_EVT_FIRST(CODEC_HW) + 1;
  static const uint16_t OVERTEMP    = HM_EVT_FIRST(CODEC_HW) + 2;
  static const uint16_t READY       = HM_EVT_FIRST(CODEC_HW) + 3;  // codec up → boot can complete
  static const uint16_t BUTTON_WAKE = HM_EVT_FIRST(CODEC_HW) + 4;  // PTT / action button (manual session)
  static const uint16_t BUTTON_MUTE = HM_EVT_FIRST(CODEC_HW) + 5;  // privacy mute toggle
  static const uint16_t LAST        = HM_EVT_FIRST(CODEC_HW) + 5;
}}
} // namespace hermes
```

### 14.7 `EventMap` Dispatch — id → handler (the "process it")
Each module owns an `EventMap`: a table from `CMsg.id` to a member-function handler. `Execute()` replaces a giant switch; registration is declarative in the module constructor. This is the TradingAlpha `EventMapTemplate`, retyped for the 16-bit Hermes IDs.

```cpp
template <class T>
class EventMap {
public:
    typedef void (T::*Handler)(const CMsg*);
    void Add(uint16_t id, Handler h)         { map_[id] = h; }
    void Erase(uint16_t id)                  { map_.erase(id); }
    int  Execute(uint16_t id, const CMsg* m) {              // 1 = handled, 0 = no handler
        auto it = map_.find(id);
        if (it == map_.end()) return 0;
        (static_cast<T*>(this)->*(it->second))(m);
        return 1;
    }
private:
    std::map<uint16_t, Handler> map_;
};
```

A module = `MsgBus` (transport, §14.8) + `EventMap` (dispatch). Inbound commands write **atomics** into `SharedFrameworkContext` (§3); the RT island reads them next quantum — a command never calls into the node:

```cpp
class AudioCoreModule : public MsgBus, public EventMap<AudioCoreModule> {
public:
    AudioCoreModule() {
        Add(hermes::_AudioCore::cmd::SET_MODE,       &AudioCoreModule::onSetMode);
        Add(hermes::_AudioCore::cmd::DUCK_PLAYBACK,  &AudioCoreModule::onDuck);
        Add(hermes::_AudioCore::cmd::FREEZE_ADAPT,   &AudioCoreModule::onFreezeAdapt);
        Add(hermes::_AudioCore::cmd::RESET_PIPELINE, &AudioCoreModule::onResetPipeline);
        Add(hermes::_AudioCore::cmd::QUERY_STATE,    &AudioCoreModule::onQueryState);
    }
    int ProcessMsg(const CMsg* m) override { return Execute(m->hdr.id, m); }  // recv thread → dispatch
private:
    void onSetMode(const CMsg* m)       { atomic_store(&g_ac->shared->currentMode, *(EngineMode*)m->pBody); }
    void onDuck(const CMsg* m)          { atomic_store(&g_ac->shared->playbackVolume, ((DuckCmd*)m->pBody)->target); }
    void onFreezeAdapt(const CMsg* m)   { atomic_store(&aecState.adaptFrozen, *(bool*)m->pBody); }
    void onResetPipeline(const CMsg*)   { atomic_store(&g_ac->shared->lastError, HW_ERR_TIMEOUT_VIOLATION); }
    void onQueryState(const CMsg* m)    { /* fill reply, SignalSync(m->hdr.src, m->hdr.syncRequestId, &snap, …) */ }
};
```

### 14.8 The Symmetric RT Boundary (two-tier bridge)
The MsgBus uses `std::map`, `std::mutex`, `malloc`, and SysV mq — **all banned on the RT path** (§11.4). It therefore sits **one hop behind** the RT island on *both* directions, crossed only by lock-free primitives. This reuses §5.2 (ring) and §3 (atomics) verbatim:

```
  RT island ──(lock-free ring, §5.2)──► Forwarder ──(MsgBus.SendMsg)──► other modules     [RT → WORLD]
  other modules ──(MsgBus cmd)──► EventMap handler ──(atomic_store)──► SharedFrameworkContext
                                                              └──(RT reads each quantum)──► RT island  [WORLD → RT]
```

* **RT → world:** the node only ever pushes a POD `AudioEventMsg` into the §5.2/§13 lock-free ring. The non-RT **forwarder** (an LP 5–7 background worker, §2.2) drains the ring, wraps each event as a `CMsg` (stamping `prio` from the §13 lane), and `SendMsg`s it. The mutex/malloc/mq all run on the forwarder, never on the data-loop.
* **World → RT:** an inbound `CMsg` lands in the recv thread → `EventMap::Execute` → handler writes an **atomic** into `SharedFrameworkContext`. The island reads those atomics at the top of `process()`. No mutex, no allocation crosses into the node.

This is the same single-consumer discipline as §13.6 — extended across the process boundary.

### 14.9 Data/Control Bridge — PipeWire Threading Contract
A PipeWire client has **two threads**, and only the non-RT one may touch the MsgBus or the PipeWire API:

| Thread | Runs | May call |
|--------|------|----------|
| **data-loop** (RT) | the RT island `process()` | atomics + lock-free ring **only** — never `MsgBus`, never `pw_*` |
| **main-loop** (`pw_thread_loop`, non-RT) | `state_changed`, `param_changed`, registry `global`/`global_remove`, errors | `MsgBus.SendMsg` directly (it is non-RT) |

* **PipeWire-native events → MsgBus** are emitted from the main-loop: device unplug → registry `global_remove` → `SendMsg(_CodecHw::evt::UNPLUGGED)` (maps §6.2 faults onto PipeWire callbacks); xrun/error → `evt::XRUN`.
* **Commands that change DSP behavior** (mode, freeze, reset) take the atomic path of §14.8 — no `pw_*` call.
* **Commands that change the PipeWire graph** (re-route per `EngineMode`, switch sink, change quantum) must be **marshalled onto the PipeWire loop** — the handler runs on the MsgBus recv thread, so calling `pw_*` directly is a data race:

```cpp
pw_thread_loop_lock(loop);
    pw_stream_set_control(playbackStream, SPA_PROP_volume, 1, &duckGain, 0);   // or relink ports
pw_thread_loop_unlock(loop);
// equivalently: pw_loop_invoke(pw_main_loop_get_loop(loop), do_graph_change, …);
```

**Rule of thumb:** out of the RT data-loop → lock-free ring + eventfd; into PipeWire → `pw_thread_loop_lock` / `pw_loop_invoke`; never a mutex on the data-loop, never a bare `pw_*` from a MsgBus thread.

### 14.10 Reference Delay & Barge-In under PipeWire
* **Report vs apply.** PipeWire *reports* the coarse transport delay via `SPA_PARAM_Latency` / `pw_time.delay` (seeds §4.3.2c stage-1) and gives a single sample-position timeline; it does **not** apply alignment. The §4.3.2 alignment ring, fractional interpolation, and ERLE re-lock stay **inside the AEC node**.
* **Barge-in invariant.** A duck is a **gain** change, not a **timing** change → `D_bulk` is invariant across barge-in; PipeWire's reported delay stays valid; no re-lock is caused by ducking (the §4.3.2d "sudden jump" triggers are underrun/route/SR changes, not a fade).
* **Duck upstream of the tap.** Apply the §5.1.3 fader as a **stream volume / in-node gain upstream of the reference tap** so the sink monitor (the AEC reference) sees the ducked signal — satisfying the §4.3.2e post-fader requirement by construction. Never duck via device/codec hardware volume downstream of the tap, or AEC over-subtracts during the barge-in.
* **No pipeline-depth term.** The single-node graph (§14.3) runs in one quantum, so the §10.8 "fold pipeline depth into `D`" caveat does not apply.

### 14.11 Timing Design under PipeWire
The §1.1 two-timescale model maps onto PipeWire's clock, with ownership shifting from a self-driven IRQ loop to PipeWire's quantum:

| §1.1 timing element | Under PipeWire |
|---|---|
| 5 ms audio block | the **quantum** — pinned and **locked**: `default.clock.quantum/min/max = 240`, `node.lock-quantum = true`, `api.alsa.period-size = 240`. |
| Hard 5 ms deadline (§6.1) | the quantum period; **soft-abort re-implemented inside `process()`**: `if (now_ns() − pos->clock.nsec > budget*0.8) soft_mute()`. |
| 1 ms control scheduler (§5, LP 4) | a **separate timer thread outside PipeWire** (PipeWire ticks every 5 ms, not 1 ms), aligned to the PipeWire clock. |
| `hardwareTimestampNs` (§3) | `spa_io_position.clock.nsec` + `.position` (also the `CLOCK_ANCHOR` payload). |
| Clock-drift PI + ASRC (§5) | PipeWire DLL + adaptive resampler (own the cross-device drift). |
| AEC reference delay `D` (§4.3.2) | seeded from PipeWire-reported `clock.delay`; tracked by the AEC node. |

**Scheduling note.** PipeWire is **timer-based by default**, not IRQ-driven — the DMA IRQ advances the ring, but the graph wakeup is a `timerfd` in the data-loop's epoll. For near-IRQ cadence set `api.alsa.period-size = quantum`, `api.alsa.headroom = 0`, `api.alsa.disable-batch = true`; the wake still flows through PipeWire's RT data-loop, not a bare handler. Run that data-loop `SCHED_FIFO` on an isolated big core (§9.5), and pin the island's self-claim pool (§11.2) alongside it — you now coordinate **two** thread systems (PipeWire's data-loop + the island's pool) on the big cluster, so the §11.3 affinity discipline is mandatory.

---

## 15. Hermes System State Machine (Session Orchestration)

The §14 catalog defines *what* can be said; this section defines *when*. The overall goal — an LLM-interactive voice turn (**wake → capture → think → speak → barge-in → listen**) with fault recovery — is governed by a single **Session FSM owned by `SUPERVISOR`**. It is the only place that decides "what happens next"; every other module is a mechanism it drives via commands and observes via events. This keeps orchestration centralized, observable, and testable, exactly as the §13.6 single-consumer argument prescribes — now at the system level.

### 15.1 Two-Level State Model
Two state machines run at different altitudes and must not be conflated:

| Level | Machine | Owner | Drives |
|-------|---------|-------|--------|
| **Session** (this section) | `SessionState` — dialog lifecycle | `SUPERVISOR` | emits `_AudioCore::cmd::SET_MODE` etc. |
| **DSP** (§3) | `EngineMode` — what the RT island computes | `AUDIO_CORE` | the §10 graph + §13 triggers |

The Session FSM **commands** the DSP `EngineMode`; it never reaches into the engine. Mapping:

| `SessionState` | `EngineMode` (§3) commanded | Capture? | Playback? | Barge-in armed? | Wake armed? |
|---|---|:--:|:--:|:--:|:--:|
| `SS_INIT` | — (bring-up) | – | – | – | – |
| `SS_IDLE` | `MODE_KEYWORD_LISTENING` | – | – | – | ✅ |
| `SS_WAKE_CONFIRM` | `MODE_KEYWORD_LISTENING` | – | – | – | ✅ |
| `SS_CAPTURE` | `MODE_CLOUD_STREAMING` | ✅ | – | – | – |
| `SS_THINK` | `MODE_CLOUD_STREAMING` | – | – | – | – |
| `SS_SPEAK` | `MODE_CLOUD_STREAMING` | – | ✅ | ✅ | – |
| `SS_BARGE_DUCK` | `MODE_BARGE_IN_MUTING` | – | ducking | – | – |
| `SS_FAULT` | `MODE_SYSTEM_RESET_ERROR` | – | – | – | – |
| `SS_SHUTDOWN` | — (teardown) | – | – | – | – |

```c
typedef enum {
    SS_INIT = 0,        // boot: bring up PipeWire graph + codec
    SS_IDLE,            // keyword listening, wake armed
    SS_WAKE_CONFIRM,    // first-stage KWD hit; awaiting 2nd-stage confirm
    SS_CAPTURE,         // streaming user utterance to CLOUD (STT)
    SS_THINK,           // utterance committed; awaiting LLM/first TTS chunk
    SS_SPEAK,           // TTS playback, full-duplex, barge-in armed
    SS_BARGE_DUCK,      // user barged in; ducking → restart capture
    SS_FAULT,           // hardware/network fault; reset + recover
    SS_SHUTDOWN,        // graceful teardown
    SS_STATE_COUNT
} SessionState;
```

### 15.2 State Diagram
```
                 ┌─────────┐  CODEC.READY & graph up
                 │ SS_INIT │ ────────────────────────────┐
                 └─────────┘                              ▼
   WAKE_REJECTED / TO_WAKE_CONFIRM            ┌────────────────────────┐
        ┌────────────────────────────────────│        SS_IDLE         │◄──────────────┐
        │                                     │  (MODE_KEYWORD_LISTEN) │               │
        ▼                                     └───────────┬────────────┘               │
 ┌──────────────────┐  KWD_CANDIDATE                      │ BUTTON_WAKE / START_SESSION │ STT_NO_SPEECH
 │ SS_WAKE_CONFIRM  │◄────────────────────────────────────┘  (PTT: skip wake)          │ / TO_NO_SPEECH
 └────────┬─────────┘                                        │                          │
          │ WAKE_CONFIRMED                                    ▼                          │
          └──────────────────────────────────►┌────────────────────────┐───────────────┘
                                               │       SS_CAPTURE       │
                ┌──────────────────────────────│  (MODE_CLOUD_STREAMING)│
                │ STT_ENDPOINT / VAD_SPEECH_OFF └───────────┬────────────┘
                ▼                                           ▲ MODE_CHANGED(mute done)
       ┌────────────────┐  TTS_CHUNK[first]      ┌──────────┴───────────┐
       │    SS_THINK    │ ─────────────────────► │     SS_BARGE_DUCK    │
       │ (await LLM/TTS)│                        │ (MODE_BARGE_IN_MUTE) │
       └───────┬────────┘                        └──────────▲───────────┘
               │ TTS_CHUNK[first]                           │ BARGE_IN / DUCK + CLOUD.ABORT
               ▼                                            │
       ┌────────────────────────┐  ─────────────────────────┘
       │       SS_SPEAK         │
       │ (playback, barge armed)│ ── PLAYBACK_DRAINED & TTS_STREAM_END ──► SS_IDLE
       └────────────────────────┘

   ANY ── UNPLUGGED / OVERTEMP / CLOUD_ERROR / DISCONNECTED ──► SS_FAULT ──(recovered)──► SS_IDLE
   ANY ── SHUTDOWN ──► SS_SHUTDOWN          SS_FAULT ──(retries exhausted)──► SS_SHUTDOWN
```

### 15.3 Transition Table
`Current —[Event] (guard) / Actions (commands emitted)→ Next`. Actions use the §14.6 catalog. `*` = from any state.

| Current | Event | Guard | Actions (emit) | Next |
|---|---|---|---|---|
| `SS_INIT` | `CodecHw::READY` | graph up | `AudioCore::SET_MODE(LISTENING)`, `VoiceTrigger::ARM`, `Supervisor::READY` | `SS_IDLE` |
| `SS_IDLE` | `AudioCore::KWD_CANDIDATE` | — | arm `TO_WAKE_CONFIRM` | `SS_WAKE_CONFIRM` |
| `SS_IDLE` | `CodecHw::BUTTON_WAKE` / `Supervisor::START_SESSION` | — | `VoiceTrigger::DISARM`, `Cloud::OPEN_STREAM`, `AudioCore::START_CAPTURE`, `SET_MODE(CLOUD_STREAMING)`, arm `TO_NO_SPEECH` | `SS_CAPTURE` |
| `SS_WAKE_CONFIRM` | `VoiceTrigger::WAKE_CONFIRMED` | — | `VoiceTrigger::DISARM`, `Cloud::OPEN_STREAM`, `AudioCore::START_CAPTURE`, `SET_MODE(CLOUD_STREAMING)`, arm `TO_NO_SPEECH` | `SS_CAPTURE` |
| `SS_WAKE_CONFIRM` | `WAKE_REJECTED` / `TO_WAKE_CONFIRM` | — | — | `SS_IDLE` |
| `SS_CAPTURE` | `Cloud::STT_ENDPOINT` / `AudioCore::VAD_SPEECH_OFF` | dwell met | `Cloud::UTTERANCE_END`, `AudioCore::STOP_CAPTURE`, arm `TO_RESPONSE` | `SS_THINK` |
| `SS_CAPTURE` | `Cloud::STT_NO_SPEECH` / `TO_NO_SPEECH` | — | `Cloud::CLOSE_STREAM`, `AudioCore::STOP_CAPTURE`, `SET_MODE(LISTENING)`, `VoiceTrigger::ARM` | `SS_IDLE` |
| `SS_THINK` | `Cloud::TTS_CHUNK` | first chunk | `AudioCore::PLAY_TTS`, `AudioCore::ARM_BARGE_IN`, cancel `TO_RESPONSE` | `SS_SPEAK` |
| `SS_THINK` | `TO_RESPONSE` | — | `Cloud::ABORT`, `SET_MODE(LISTENING)`, `VoiceTrigger::ARM` | `SS_IDLE` |
| `SS_SPEAK` | `AudioCore::BARGE_IN` | — | `AudioCore::DUCK_PLAYBACK`, `Cloud::ABORT` | `SS_BARGE_DUCK` |
| `SS_SPEAK` | `AudioCore::PLAYBACK_DRAINED` | `TTS_STREAM_END` seen | `AudioCore::STOP_TTS`, `DISARM_BARGE_IN`, `SET_MODE(LISTENING)`, `VoiceTrigger::ARM` | `SS_IDLE` |
| `SS_BARGE_DUCK` | `AudioCore::MODE_CHANGED` | mute complete | `AudioCore::STOP_TTS`, `DISARM_BARGE_IN`, `Cloud::OPEN_STREAM`, `AudioCore::START_CAPTURE`, `SET_MODE(CLOUD_STREAMING)`, arm `TO_NO_SPEECH` | `SS_CAPTURE` |
| `*` | `CodecHw::UNPLUGGED`/`OVERTEMP`, `Cloud::CLOUD_ERROR`/`DISCONNECTED` | — | (entry actions of `SS_FAULT`) | `SS_FAULT` |
| `*` | `Supervisor::CANCEL_SESSION` | not INIT/SHUTDOWN | `Cloud::ABORT`, `AudioCore::STOP_TTS`, `STOP_CAPTURE`, `SET_MODE(LISTENING)`, `VoiceTrigger::ARM` | `SS_IDLE` |
| `*` | `Supervisor::SHUTDOWN` | — | (entry actions of `SS_SHUTDOWN`) | `SS_SHUTDOWN` |
| `SS_FAULT` | *entry* | — | `AudioCore::RESET_PIPELINE`, `Cloud::ABORT`, `CodecHw::RESET`, `SET_MODE(RESET_ERROR)`, arm `TO_RESET` | — |
| `SS_FAULT` | `AudioCore::MODE_CHANGED` & `CodecHw::READY` | reset done | `SET_MODE(LISTENING)`, `VoiceTrigger::ARM` | `SS_IDLE` |
| `SS_FAULT` | `TO_RESET` | retries exhausted | — | `SS_SHUTDOWN` |

**Key orchestration decisions baked in:**
* **Barge-in restarts the turn.** `SS_SPEAK →(BARGE_IN)→ SS_BARGE_DUCK →(mute done)→ SS_CAPTURE`: the user's interrupting speech becomes the next utterance — the duck (§8) and `Cloud::ABORT` (kill in-flight TTS/LLM) happen on the way. This is the central design goal.
* **TTS completion is two-signal.** Leaving `SS_SPEAK` requires **both** `Cloud::TTS_STREAM_END` (no more chunks coming) **and** `AudioCore::PLAYBACK_DRAINED` (the buffered audio finished playing) — never just one, or you cut off the tail or hang.
* **Fault is global + bounded.** Any fault from any state funnels to `SS_FAULT`, which runs the §6.2 Reset Pipeline and retries a bounded number of times before `SS_SHUTDOWN`.
* **Wake is suppressed mid-turn.** `VoiceTrigger::DISARM` on turn start, `ARM` on return to `SS_IDLE` — the device can't "wake itself" while already in a conversation.

### 15.4 Implementation — table-driven FSM driven by the `EventMap`
The FSM is **data**, not a switch forest. `SUPERVISOR`'s `EventMap` (§14.7) routes *every* inbound event to one dispatcher; the dispatcher matches `(state, evtId)` against the transition table, runs the guard, fires actions, and updates state. Because `SUPERVISOR` is the single consumer of this stream, transitions are race-free and ordered by construction (§13.6).

```c
typedef struct {
    SessionState  from;
    uint16_t      evtId;                                   // §14.6 catalog id (ANY = wildcard)
    bool        (*guard)(SupervisorCtx*, const CMsg*);     // NULL ⇒ always
    void        (*action)(SupervisorCtx*, const CMsg*);    // emit commands via MsgBus.SendMsg
    SessionState  to;                                      // SS_STATE_COUNT ⇒ self (no change)
} Transition;

#define ANY 0xFFFF

static const Transition kTable[] = {
  { SS_INIT,         _CodecHw::evt::READY,            g_graphUp,   a_enter_idle,    SS_IDLE        },
  { SS_IDLE,         _AudioCore::evt::KWD_CANDIDATE,  NULL,        a_arm_confirm,   SS_WAKE_CONFIRM},
  { SS_IDLE,         _CodecHw::evt::BUTTON_WAKE,      NULL,        a_start_turn,    SS_CAPTURE     },
  { SS_WAKE_CONFIRM, _VoiceTrigger::evt::WAKE_CONFIRMED, NULL,     a_start_turn,    SS_CAPTURE     },
  { SS_WAKE_CONFIRM, _VoiceTrigger::evt::WAKE_REJECTED,  NULL,     NULL,            SS_IDLE        },
  { SS_CAPTURE,      _Cloud::evt::STT_ENDPOINT,       NULL,        a_commit_utt,    SS_THINK       },
  { SS_CAPTURE,      _Cloud::evt::STT_NO_SPEECH,      NULL,        a_back_to_idle,  SS_IDLE        },
  { SS_THINK,        _Cloud::evt::TTS_CHUNK,          g_firstChunk,a_start_speak,   SS_SPEAK       },
  { SS_SPEAK,        _AudioCore::evt::BARGE_IN,       NULL,        a_duck_abort,    SS_BARGE_DUCK  },
  { SS_SPEAK,        _AudioCore::evt::PLAYBACK_DRAINED, g_ttsEnded,a_back_to_idle,  SS_IDLE        },
  { SS_BARGE_DUCK,   _AudioCore::evt::MODE_CHANGED,   g_muteDone,  a_start_turn,    SS_CAPTURE     },
  { ANY,             _CodecHw::evt::UNPLUGGED,        NULL,        NULL,            SS_FAULT       },
  { ANY,             _Supervisor::cmd::CANCEL_SESSION, g_inTurn,   a_cancel,        SS_IDLE        },
  { ANY,             _Supervisor::cmd::SHUTDOWN,      NULL,        NULL,            SS_SHUTDOWN    },
  // … (timeouts TO_*, FAULT recovery, etc. — full table per §15.3)
};

// One dispatcher for ALL inbound events — wired into SUPERVISOR's EventMap (§14.7).
void Fsm_OnEvent(SupervisorCtx* sx, const CMsg* m) {
    for (const Transition* t = kTable; t < kTable + ARRAY_LEN(kTable); ++t) {
        if ((t->from == sx->state || t->from == ANY) && t->evtId == m->hdr.id) {
            if (t->guard && !t->guard(sx, m)) continue;          // guard failed → try next row
            if (t->action) t->action(sx, m);                     // emit commands (SendMsg)
            if (t->to != SS_STATE_COUNT && t->to != sx->state) {
                Fsm_Exit(sx, sx->state); SessionState from = sx->state;
                sx->state = t->to; Fsm_Enter(sx, t->to);         // run entry actions (e.g. SS_FAULT reset)
                Bus_Broadcast(_Supervisor::evt::STATE_CHANGED, from, t->to);
            }
            return;                                              // first match wins
        }
    }
    // no transition for (state, evt): log + drop (most events are no-ops in most states)
}
```

`a_start_turn`, `a_duck_abort`, etc. are the only places that call `MsgBus.SendMsg` — every command leaving `SUPERVISOR` originates in a transition action, so the side-effects of the FSM are fully enumerated and auditable.

### 15.5 Timeouts & Guards
Timeouts are first-class events (`_Supervisor::internal::TO_*`, §14.6) posted by `SUPERVISOR`'s own timer onto its inbound queue, so they flow through the *same* `Fsm_OnEvent` path as wire events — no special-casing.

| Timer | Armed on entry to | Fires → | Purpose |
|---|---|---|---|
| `TO_WAKE_CONFIRM` | `SS_WAKE_CONFIRM` | `SS_IDLE` | abandon an unconfirmed candidate |
| `TO_NO_SPEECH` | `SS_CAPTURE` | `SS_IDLE` | user woke but said nothing |
| `TO_RESPONSE` | `SS_THINK` | `SS_IDLE` (or error tone) | cloud/LLM too slow |
| `TO_SESSION_MAX` | turn start | `SS_FAULT`/`SS_IDLE` | hard cap on a single turn |
| `TO_RESET` | `SS_FAULT` | `SS_SHUTDOWN` | bounded recovery retries |

Guards (`g_*`) encode the non-event conditions: `g_firstChunk` (this is the first `TTS_CHUNK` of the response), `g_ttsEnded` (`TTS_STREAM_END` already seen — the two-signal completion), `g_muteDone`, `g_inTurn` (state ∉ {INIT, IDLE, SHUTDOWN}).

### 15.6 Why the FSM lives in `SUPERVISOR`, not `AUDIO_CORE`
`AUDIO_CORE` is hard-RT and must stay a pure mechanism (§14.4) — embedding dialog policy there would put network/LLM timing concerns on the audio data-loop. By hoisting the FSM into the non-RT `SUPERVISOR` and crossing the RT boundary only via the §14.8 symmetric bridge (events up via the lock-free ring, commands down via atomics), the orchestration logic can be complex, evolve freely, and even crash-restart **without touching the audio path**. The RT island keeps streaming throughout; only the *decisions* live in the FSM.

### 15.7 SUPERVISOR Threading — single serialized FSM + worker pool
`SUPERVISOR` is the **control brain, not a hard-RT deadline keeper** — the millisecond-critical barge-in duck is **local to ABOX** (§13.3), reached by its own VAD→fader, *not* a Supervisor round-trip. So the control process is prompt but not hard-RT, structured in three roles:

| Role | Threads | Priority | Duty |
|------|---------|----------|------|
| **① Recv / IPC intake** | 1 | highest in-process (`< ABOX` RT) | drain the mq **by priority lane** into an in-process FIFO; **no handling** (the overridable `RecvMsgTask`) |
| **② FSM executor** | **1 (serialized)** | elevated | pop the FIFO, run transitions **in order** — the **only** thread that touches `state_` ⇒ race-free, lock-free, no pool |
| **③ Worker pool** | few | `SCHED_OTHER` | long Supervisor-**local** tasks (config, persistence, metrics); results posted **back as events** to ② |

**Invariants:**
* **The FSM is never fanned across a pool.** A pool would race on `state_` and reorder transitions; one serialized consumer gives the §13.6 single-consumer guarantee for free (no lock on the hot path).
* **A transition is O(µs)** — check state → async `SendMsg` (non-blocking) → change state. It must never block.
* **Waiting is a state, not a blocked thread.** Long work (cloud STT/LLM/TTS, codec reset) lives in *other processes* reached by async `SendMsg`; its result returns as another event to ②. Supervisor-local long work goes to ③ and likewise returns as an event.
* **Priority is delivered by the mq lane**, not raw thread priority — a `PRIO_URGENT` barge-in/wake `CMsg` is dequeued by ① ahead of everything, so the FSM reacts to it first.

---

## 16. Voice-Trigger / ABOX Split & Post-Trigger Audio Handoff

This section fixes the **process boundary** between the always-on voice-trigger system (**VTS**, `ModuleId 3`) and the DSP engine (**ABOX** = `AUDIO_CORE`, `ModuleId 2`), and specifies how the user's utterance audio reaches the cloud **after** a wake. It supersedes the wake-related parts of §4.4, §7, and §15 where noted.

### 16.1 Ownership — one clean source, two roles
* **ABOX is the sole producer of clean mono** (SRC→AEC→beamform, §4). Nothing else processes mic audio.
* **VTS is a separate process** running the **complete always-on keyword detection** (§7 feature extraction → scoring → threshold/debounce). It is a **PipeWire client capturing the mic source DIRECTLY** — its own capture stream on the ALSA source, **independent of ABOX** (never via ABOX's output) — and it **owns the rolling raw-mic pre-roll buffer**. ABOX keeps **no lookback** — VTS is the sole owner of recent-audio history. Because VTS no longer depends on ABOX's output, **ABOX may stay parked in `SS_IDLE`** (it need not run to feed VTS) — a power win consistent with §2.3/§11.2.
* **`CLOUD_CONNECTOR`** (`ModuleId 5`, renamed from `STT_CLOUD`) is an **on-target proxy**: a PipeWire client locally, a network client (WebSocket/gRPC) to the remote STT/LLM/TTS. **PipeWire never reaches the cloud** — only the connector's network socket does.

> **Reconciliation (supersedes earlier sections):** (a) KWD moves out of the ABOX RT island — §4.4 perception keeps **VAD only** (barge-in §8 still needs the in-RT AEC residual); the §7 detector lives in VTS. (b) Catalog: `AUDIO_CORE::evt::KWD_CANDIDATE` is retired (reserved); VTS emits a single `VOICE_TRIGGER::evt::WAKE_CONFIRMED`. (c) FSM §15: `SS_WAKE_CONFIRM` and `TO_WAKE_CONFIRM` are retired — `SS_IDLE →(WAKE_CONFIRMED)→ SS_CAPTURE` directly, since VTS does full detection in-process. (d) Data plane §14.3: **VTS and ABOX are independent PipeWire capture clients on the same mic source** — VTS captures the mic **directly** (raw), never via ABOX's output.

### 16.2 Interrupt (control) vs. stream (data) — strictly separated
The wake is an **interrupt** on the control plane; the utterance is a **stream** on the data plane. They never mix:

```
[ALSA mic source] ─PipeWire─┬─► VTS (RAW mic) ── keyword detect ──► WAKE_CONFIRMED  ◄── INTERRUPT (control)
                            │      owns rolling RAW pre-roll ring   {wake_pos, capture_from_pos, epoch}
                            │                                                ▼
                            │                          SUPERVISOR FSM ──► ABOX: START_CAPTURE
                            │                                              CLOUD_CONNECTOR: OPEN_STREAM
                            └─► ABOX (RAW mic) ── DSP (AEC/beamform) ──► clean mono ──► CLOUD_CONNECTOR ─net─► STT/LLM/TTS
                                   ▲ own live mic tap (direct)                ▲ live utterance
       VTS RAW pre-roll ring ─(zero-copy)─► ABOX  (pre-wake history ABOX wasn't running for — §16.3)
```

VTS and ABOX **independently capture the mic** (same source, same PipeWire `sample_pos` clock). VTS only fires the interrupt + supplies the raw pre-roll; ABOX does the utterance DSP and is the single egress to `CLOUD_CONNECTOR`. The **history** (pre-roll) predating the interrupt comes from VTS's raw ring; the **live** continuation comes from ABOX's own raw mic tap — ABOX processes both through DSP into clean mono.

### 16.3 Pre-Roll Handoff — Shared-Memory Ring (zero-copy, latency-first)
KWD detection has latency (~hundreds of ms rolling window, §7.1): when `WAKE_CONFIRMED` fires, the wake word is already past and the user's command may have begun. Capturing only from the interrupt forward would **clip the command**. VTS therefore retains a rolling pre-roll; ABOX rewinds into it. **Transport: a persistent lock-free SPSC ring in named shared memory** (chosen over a one-shot `MEM_SHM` `CMsg` because the live path must avoid every avoidable hop — the ring makes the pre-roll instantly available with no per-trigger alloc/copy).

```c
// /hermes.preroll — mapped by VTS (sole producer) and ABOX (sole consumer). sample_pos-indexed.
#define PREROLL_RING_SAMPLES (16000 * 3)     // 3 s @ 16 kHz mono ≥ pre-roll + handoff headroom
#define PREROLL_GUARD        1600            // 100 ms overwrite guard band

typedef struct {
    _Atomic uint64_t writePos;               // running sample_pos of next write (producer cursor)
    _Atomic uint32_t epoch;                  // bumped on xrun/reset → invalidates stale windows
    int16_t          pcm[PREROLL_RING_SAMPLES];   // RAW mic (VTS's direct tap); VTS writes, ABOX reads → DSP
} PrerollRing;                                     // multi-channel variant: interleave for ABOX beamform

// VTS producer — its rolling buffer LIVES here (no extra copy), one writer:
static inline void Preroll_Write(PrerollRing* r, const int16_t* blk, int n) {
    uint64_t w = atomic_load_explicit(&r->writePos, memory_order_relaxed);
    for (int i = 0; i < n; ++i) r->pcm[(w + i) % PREROLL_RING_SAMPLES] = blk[i];
    atomic_store_explicit(&r->writePos, w + n, memory_order_release);   // publish
}

// ABOX consumer — on the WAKE_CONFIRMED interrupt {capture_from_pos, epoch}:
static int Preroll_Read(PrerollRing* r, uint64_t from, uint32_t epoch,
                        uint64_t* out_from, uint64_t* out_to) {
    uint64_t w = atomic_load_explicit(&r->writePos, memory_order_acquire);
    if (atomic_load_explicit(&r->epoch, memory_order_acquire) != epoch) return 0; // discontinuity → skip pre-roll
    if (w - from > PREROLL_RING_SAMPLES - PREROLL_GUARD)                           // aged out → clamp oldest-safe
        from = w - (PREROLL_RING_SAMPLES - PREROLL_GUARD);
    *out_from = from; *out_to = w;                                                 // stream [from .. w) zero-copy
    return 1;
}
```

**Two correctness guards (the cost of the zero-copy ring):**
1. **Overwrite race.** VTS keeps writing live while ABOX drains the pre-roll. The ring is sized ≫ pre-roll so a wrap cannot reach the window mid-read; the `GUARD` band + the `acquire`-loaded `writePos` validate it. Single-producer / single-consumer + atomic cursor ⇒ lock-free, no mutex (consistent with §11.4).
2. **The seam (raw-domain, simpler).** VTS's ring and ABOX's own live mic tap are the **same source on one PipeWire `sample_pos` timeline**. On wake ABOX reads the raw pre-roll `[from .. writePos)` from the ring, then continues from its **own live raw mic tap** at the same `sample_pos` — one continuous raw stream split between VTS's history and ABOX's live capture, so no gap and no overlap. ABOX feeds `[raw pre-roll ⧺ raw live]` through its DSP → clean mono to `CLOUD_CONNECTOR` (no round-trip). **AEC is a no-op on the pre-roll**: the wake occurs in `SS_IDLE` (no playback), so the pre-roll carries no echo — only beamform/denoise apply.

### 16.4 `WAKE_CONFIRMED` payload
```c
typedef struct {
    uint64_t wake_pos;          // sample_pos of the detected keyword's end (PipeWire timeline, §14)
    uint64_t capture_from_pos;  // where ABOX should begin pre-roll = wake_pos − pre_roll_margin
    uint32_t epoch;             // PrerollRing.epoch at detection — ABOX validates against current
} WakeConfirmedBody;
```
VTS sets `capture_from_pos` from its own detection latency so the user's command is never clipped; ABOX clamps it to the oldest sample still safely in the ring (§16.3 guard 1).

### 16.5 Post-trigger sequence (end-to-end)
```
1. VTS  (always-on): keyword detected at sample_pos = wake_pos
2. VTS  → WAKE_CONFIRMED{wake_pos, capture_from_pos, epoch}      [INTERRUPT, control plane]
3. SUPERVISOR FSM: SS_IDLE → SS_CAPTURE;  emit AUDIO_CORE::START_CAPTURE, CLOUD_CONNECTOR::OPEN_STREAM,
                   VOICE_TRIGGER::DISARM   (suppress self-wake during the turn)
4. ABOX: Preroll_Read() → stream [capture_from_pos .. writePos) ZERO-COPY from /hermes.preroll
5. ABOX: splice at sample_pos → stream OWN live clean mono → CLOUD_CONNECTOR (PipeWire, on target)
6. CLOUD_CONNECTOR: forward [pre-roll ⧺ live] over the network to remote STT
   … turn proceeds per §15 (SS_CAPTURE → SS_THINK → SS_SPEAK …); on return to SS_IDLE: VOICE_TRIGGER::ARM
```

The utterance is therefore **processed in ABOX** (DSP + egress); VTS contributes only the interrupt and the pre-roll history; the cloud is reached solely by `CLOUD_CONNECTOR`'s network socket.

---

## 17. Design Decisions Summary & Critical Paths

This section records the deployment-architecture decisions taken for Hermes (the result of the PipeWire/MsgBus design review) and pins the two latency-critical paths the whole system is optimized around.

### 17.1 Architecture Decisions
| # | Decision | Rationale | Ref |
|---|----------|-----------|-----|
| **D1** | **PipeWire = data plane, MsgBus = control plane** | samples vs. decisions strictly separated; PipeWire owns clock/device/routing, MsgBus owns cmd/evt | §14.1 |
| **D2** | The §1–§13 engine is hosted as **one PipeWire SPA node** (the "RT island") | preserves zero-alloc/lock-free/cache-align *inside* the node; PipeWire supplies clock + zero-copy I/O *around* it | §14.4 |
| **D3** | Cross-process transport = **`ModuleId → /hermes.mod.<id>` POSIX mq**, not the in-process pointer registry | the TradingAlpha `mBusRegistry` holds per-process `MsgBus*` pointers → cannot route across Hermes's 6 processes; a named mq makes `ModuleId` a true global address, and mq priority maps to the §13 lanes | §14.5.2 |
| **D4** | **VTS and ABOX are independent processes**, each with its **own direct mic capture** | VTS is truly always-on and decoupled (survives ABOX reset; lets ABOX idle-park); neither feeds the other its stream | §16.1 |
| **D5** | **Keyword detection lives entirely in VTS** (raw mic); ABOX keeps **VAD only** | wake detection is latency-tolerant and non-RT; barge-in VAD needs the in-RT AEC residual and stays in the island | §16.1, §8 |
| **D6** | Pre-roll handoff = **VTS-owned raw-mic shm ring** (Option B), zero-copy | latency-first: no per-trigger alloc/copy; ABOX processes `[pre-roll ⧺ live]`; AEC is a no-op on the (idle, echo-free) pre-roll | §16.3 |
| **D7** | **`CLOUD_CONNECTOR` is an on-target proxy** (PipeWire client ↔ network); **PipeWire never reaches the cloud** | the remote STT/LLM/TTS is reached only by the connector's socket | §16.1 |
| **D8** | **Session FSM in SUPERVISOR** drives the DSP `EngineMode`; **barge-in and KWD are the two interrupts** that pivot it | central, observable orchestration; the RT path stays pure mechanism | §15 |
| **D9** | **Node processing + data path implemented in C** (C11 `abox_node` vtable); control plane stays C++ | a C++ `virtual` already *is* a vtable (no RTTI), so no perf delta — the choice is readability/maintainability of the hot path | §18.1 |
| **D10** | **Superseded → adopted the monolithic Core-Proportional Buffer Pool** (ONE `pw_filter` whose `on_process` walks the whole graph). Initially the engine was multi-node (a `pw_filter` per stage); on the decision to wire the buffer pool, the live path moved to Model B. The earlier review concerns are tracked: input-drop was replaced by **Soft-Mute** (D11), and the worker-pool/firewall caveats are in §18.2. | §18.0, §18.1 |
| **D11** | Per-block deadline overrun → **Soft-Mute zero-fill**, never input frame-drop | abandoning the late partial keeps the output glitch-free *and* preserves the AEC reference ring + barge-in capture | §6.1, §18.2 |
| **D12** | A **declarative `active_pipeline_mask`** gates each stage in the tick loop (include/skip by use-case) | dynamic bypass (e.g. AEC/Beam off in KeywordListening) with no re-link and no runtime alloc | §10.9, §18.1 |

### 17.2 The Two Critical Paths (system KPIs)
Barge-in and keyword detection are **the** key paths; everything else is sized to not disturb them. They sit at **different RT tiers by necessity**:

| | **Keyword detection (KWD)** | **Barge-in** |
|---|---|---|
| Owner / tier | **VTS** — own process, non-RT, always-on | **ABOX RT island** — hard-RT, in the data-loop |
| Input | own raw mic tap (PipeWire) | live post-AEC residual (§8.1) |
| Why there | tolerant of a separate process; full §7 model | needs the residual + can't survive a cross-process hop |
| Interrupt | `VOICE_TRIGGER::evt::WAKE_CONFIRMED` | `AUDIO_CORE::evt::BARGE_IN` (`PRIO_URGENT`, §13.1) |
| Latency mechanism | zero-copy raw pre-roll ring → no command clipping (§16.3) | URGENT lane ≤ 1 ms; duck-start ≤ 1 ms; silence ≤ 12 ms (§8.2) |
| FSM edge | `SS_IDLE → SS_CAPTURE` | `SS_SPEAK → SS_BARGE_DUCK → SS_CAPTURE` |

> **Budgets (targets — confirm against product spec):** barge-in: speech-onset → duck-start ≤ 1 ms, → silence ≤ 12 ms, → capture restart ≤ 1 block (5 ms). KWD: pre-roll lossless (no clip); wake → first-STT-byte budget TBD. These two are the **primary integration tests** (`test/integration/barge_in_e2e`, `kwd_wake_e2e`).

### 17.3 Reconciliation Notes (supersedes earlier text)
* §4.4 / §7: KWD removed from the ABOX RT island (perception = **VAD only**); the detector lives in VTS (§16).
* §15: `SS_WAKE_CONFIRM` and `TO_WAKE_CONFIRM` retired — `SS_IDLE →(WAKE_CONFIRMED)→ SS_CAPTURE` directly.
* Catalog: `AUDIO_CORE::evt::KWD_CANDIDATE` retired (id reserved); `STT_CLOUD` renamed `CLOUD_CONNECTOR`.

---

## 18. As-Built Realization — C Data Plane (`app/audio_core/abox/`)

This section records what is **actually implemented** for the node-processing data path, distinct from the still-aspirational parts of §10/§11. The data path is C (C11, `_Atomic`); the control plane (SUPERVISOR FSM, MsgBus, the PipeWire glue) stays C++ (D9).

### 18.0 As-built data-plane diagram (Figure 18-A)

`✅` built/tested · `⚠` stub or not-yet-wired.

```
┌──────────────────────────────────────────────────────────────────────────────┐
│  Linux Kernel — ALSA ASoC / I2S DMA            (5 ms period, IRQ → A76 cpu4)    │
└───────────────────────────────────┬────────────────────────────────────────────┘
                                     │ planar float buffers (per quantum)
                                     ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│  PipeWire (libpipewire-0.3)   on_process → pw_filter_get_dsp_buffer(port,n)     │
└───────────────────────────────────┬────────────────────────────────────────────┘
                                     │  ⚠ live wiring TODO: PwStage still drives C++ dsp::Node
                                     ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│  COORDINATOR ✅ hermes_pipeline_process_tick()              buffer_pipeline.c    │
│    slot 0 (cpu6) ─ abox_frame ─ _Atomic core_in_progress[0] ◄┐ firewall          │
│    slot 1 (cpu7) ─ abox_frame ─ _Atomic core_in_progress[1] ◄┘ (soft-drop=no Xrun)│
│    busy?─yes─►SOFT-DROP (drops++)   busy?─no─►alias in_chan (ZERO-COPY)→rotate→↓  │
│    mode ◄─ _Atomic swap (control plane)            abox_param_store ✅ (§4.2)      │
└───────────────────────────────────┬────────────────────────────────────────────┘
                                     │ mask = abox_active_mask(mode)  ✅ §10.9
                                     ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│  MASK-GATED CASCADE ✅ abox_graph_tick()  (in place — (mask&bit)?process:SKIP)   │
│   ┌───────┐  ┌────────┐    ┌────────────┐  ┌───────┐                             │
│   │SRC ⚠  │─►│AEC ✅↕  │──►│BEAMFORM ⚠  │─►│SES ⚠  │──► out                      │
│   └───────┘  └───┬────┘ 2ch└────────────┘1c└───────┘                             │
│                  │ aligned far-end                                              │
│         ┌────────▼─────────────┐     ┌───────────────────────────────┐          │
│         │RefManager ✅ §4.3.2   │     │WorkerPool ✅ §11.2            │          │
│         │ring+VI-Sense, bulk D │     │abox_cmd/done_ctr, A76 self-claim│        │
│         └──────────────────────┘     └───────────────────────────────┘          │
└───────────────────────────────────┬────────────────────────────────────────────┘
                                     │ EGRESS: single memcpy → out_chan[c]
                                     ▼     (deadline overrun? → abox_soft_mute ✅ §6.1)
                              PipeWire out → DAC / smart-amp
```

### 18.1 Module map

| File | Realizes | Notes |
|------|----------|-------|
| `abox_node.h` | §3 / §12 node contract | `abox_frame` (PLANAR `chan[c]` pointers → zero-copy in-place), `abox_node_ops` vtable, `abox_node`, `abox_stage`, `abox_graph`, `abox_soft_mute()` |
| `abox_routing.c` | §4.0.1 routing matrix + §10.9 mask | gain table per `abox_mode`; `abox_active_mask()` is the `active_pipeline_mask` (gain>0 ⇒ bit set), read once per block |
| `abox_graph.c` | §10.9 tier-1 tick | walks the static stage array, evaluates the mask in the loop, runs or **skips** each stage in place — no re-link, no alloc |
| `reference_manager.c/.h` | §4.3.1–§4.3.2 | post-fader loopback ring, bulk delay + fractional interp; **VI-Sense preferred over mixer output** per sample |
| `worker_pool.c/.h` | §11.2 self-claim pool | `abox_cmd` jobs, `next_job`/`done_ctr`/`generation`, hybrid spin/park, spin-wait join + deadline abort, A76 pinning |
| `buffer_pipeline.c/.h` | Core-Proportional Buffer Pool (§2.2/§3.1 of the pool SDD) | per-A76-core frame slot + `_Atomic core_in_progress[]` firewall; zero-copy ingest (slot aliases driver buffer), mask-gated in-place cascade, single egress copy; soft-drop on overrun |
| `param_store.h` | §4.2 async param swap | lock-free double-buffer; control fills the inactive slot, publishes with a release store (4-mic↔1-mic profile, coeff sets) |
| `abox_nodes.c/.h` | §4 nodes | `src`/`aec`/`beamform`/`ses` as vtable instances + factory; AEC pulls the aligned reference. DSP kernels (PBFDAF, MVDR) are stubs marked `TODO` |
| `harness_offline.c` | §5 | deterministic offline harness — runs the chain over synthetic frames, **no PipeWire** |
| `abox_selftest.c` | — | assert-based C self-test under CTest (mask, gating, ref alignment, pool, param store) |

**Live wiring (D9, done).** `hermes_abox` is a **monolithic binary** hosting **ONE PipeWire node** (`pw_filter`) whose `on_process` (`abox_main.cpp::abox_block`) drives `hermes_pipeline_process_tick` — the buffer-pool coordinator that walks the **multiple `abox_node`s** (src→aec→beamform→ses) in process. The C++ `dsp::Node`/`PwStage` per-filter layer has been deleted. This is "one PipeWire node, many DSP nodes, one binary" (Model B).

### 18.2 Buffering & overflow handling

Two complementary mechanisms are implemented; a third (the §10 inter-stage SPSC queue) remains spec-only.

**(a) Core-Proportional Buffer Pool — `buffer_pipeline.c` (implemented).** The coordinator (`hermes_pipeline_process_tick`, SDD §3.1) owns one frame slot per reserved A76 core (`HERMES_NUM_WORKER_CORES`, default 2 → cpu6/cpu7) plus a lock-free `_Atomic core_in_progress[]` firewall. Each period it picks the next slot, **zero-copy-aliases** the driver buffer into it (no ingest memcpy), runs the mask-gated cascade in place, copies the result out, then releases the slot. If the target slot is still in-progress from a prior period, the period is **soft-dropped** (return 1, `drops++`) — the driver ping-pong keeps progressing so **no ALSA Xrun** occurs; recovery stays in userspace.

> **Sync vs. async caveat (important).** When the cascade runs **inline** on the caller (`pool == NULL`), the slot's busy flag is set and cleared within the same callback, so the firewall is a structural guard that rarely fires — the double-buffer does not yet *absorb* an overrun, it only *detects re-entrancy*. The pool genuinely double-buffers only when the cascade is **dispatched to the standing A76 pool** (`hermes_pipeline_set_pool`) and the callback returns before the worker finishes: then `core_in_progress[core]` stays true across the period boundary, the next period rotates to the *other* core's slot, and a stuck core is soft-dropped. Wiring the async (return-before-done) dispatch + an egress collect of the *previous* period's completed slot is the remaining step to make the pool absorb (not just detect) overruns — it costs the classic one-period pipeline latency (§10.8).

**(b) Soft-Mute Fallback — `abox_soft_mute` / `PwStage::block` (implemented).** On a per-block deadline overrun the late partial is abandoned and the output zero-filled (D11, §6.1) — a clean mute over a glitch, preserving the AEC reference ring and barge-in capture. The worker-pool join (`done_ctr` + deadline → `force_abort`) is the parallel-fan-out equivalent.

**(c) §10 inter-stage SPSC double/triple buffer (NOT implemented).** `QUEUE_CAPACITY = 4` pointer queues between stages, which *absorb* a transient overrun by giving the consumer slack, plus §10.5 overflow/underflow recovery. Required once a multi-stage chain runs **inside one node** across cores (§10.9 tier-3); until then the as-built path is the proportional pool (a) + mute (b).

### 18.3 Verification status
* **Native arm64** (Docker, `./scripts/build.sh test`): 9/9 CTest pass — `abox_selftest` (C: routing mask, graph gating, reference alignment, worker pool, param store, **buffer-pool firewall + rotation**) + the C++ DSP suites + the two §17.2 e2e KPIs.
* **Offline harness** (`hermes_offline_c`): runs the chain with no daemon; per-mode mask `0x00 / 0x7f / 0x3f` confirms §10.9 gating.
* **RK3588 cross-compile** (`./scripts/build.sh`): `libhermes_abox_c.a` + `hermes_offline_c` build as aarch64 ELF on the `cmake/rk3588.toolchain.cmake` cross toolchain.

### 18.4 Implementation Gap Table (design ↔ code)

Audit of this tree (✅ done · 🟡 partial · ⛔ stub/missing). Evidence is `file:line`.

| Capability | Design ref | Status | Evidence | What it blocks |
|---|---|---|---|---|
| Session FSM (8 states + transitions) | §15 | ✅ | `SessionFsm.cpp:55` (FsmLoop, all edges) | — |
| In-process priority lanes (URGENT/NORMAL/DEFERRED) | §13 | ✅ | `EventQueue.hpp:38` | — |
| Mode-adaptive static graph + `active_pipeline_mask` | §4/§10.9 | ✅ | `abox_graph.c`, `abox_routing.c` | — |
| C node vtable framework + worker pool + param store | §2.1/§11.2/§4.2 | ✅ | `abox_node.h`, `worker_pool.c`, `param_store.h` | — |
| Core-Proportional Buffer Pool + firewall | pool §3 | 🟡 | `buffer_pipeline.c` — built/tested, **sync only (detects, doesn't absorb)** | overrun absorption |
| Reference manager (ring + bulk delay + VI-Sense) | §4.3.2 | 🟡 | ring/interp real; **cross-correlation delay-lock is seed-only** | AEC convergence |
| **Cross-process transport (POSIX mq)** | §14.5 | ✅ | `MsgBus.cpp` — real `mq_open/mq_send/mq_timedreceive`, prio-inverted lanes, recv thread; test `test_msgbus` (delivery + lane order + absent-peer) | — |
| AEC PBFDAF kernel | §4.3 | ⛔ | `AecNode.hpp:46` / `abox_nodes.c` `// TODO real PBFDAF` (ramped bypass only) | echo cancellation |
| Beamformer MVDR/GSC | §4.2 | ⛔ | `BeamformNode.hpp:12` (bypass); `abox_nodes.c` (naïve 2→1 average) | spatial enhancement |
| SES (spectral suppression) | §4.4 | ⛔ | `SesNode.hpp:12` passthrough | noise suppression |
| SRC/ASRC fractional resampler (C node) | §4.1 | ✅ | `abox_nodes.c` `src_process` — drift-ratio linear interp, cross-block phase carry, identity at ratio 1.0; `abox_src_set_ratio()`; test `test_src_node` | (needs input FIFO + §5 PI loop to sustain ratio≠1) |
| Clock-drift PI loop feeding the ASRC ratio | §5 | ⛔ | no PI controller; `abox_src_set_ratio` has no driver yet | long-term AEC alignment |
| **Barge-in VAD → emit `AUDIO_CORE::evt::BARGE_IN`** | §8 | ⛔ | handler `SessionFsm.cpp:110` receives it; **nothing sends it**; no VAD node | barge-in use case |
| KWD in VTS | §7/§16 | ⛔ | `voice_trigger/main.cpp:37` TODO; loop is `pause()` | wake use case |
| Pre-roll VTS→ABOX handoff | §16.3 | 🟡 | `PrerollRing.hpp` defined; **not wired** anywhere | lossless wake capture |
| Cloud connector (STT/LLM/TTS proxy) | §16.1 | ⛔ | `cloud_connector/main.cpp:22` empty handlers; no socket | conversation turn |
| 1 ms micro-scheduler (drift PI, fader, /dev/input) | §5 | ⛔ | `codec_hw/main.cpp:30` TODO; no fader in the path | ducking, buttons, drift |
| ABOX control verbs (START_CAPTURE/DUCK/ARM_BARGE_IN/RESET) | §6.2/§16 | ⛔ | `abox_main.cpp:26,37` TODO | capture/duck/reset |
| **Live path runs the C data plane (D9)** | §18.1, D9 | ✅ | `abox_main.cpp` — ONE `pw_filter` whose `on_process` (`abox_block`) drives `hermes_pipeline_process_tick` over the C nodes; the C++ `dsp::Node`/`PwStage` layer is **deleted**. Playback smoke: `hermes_abox` connects + runs the data-loop | — |

### 18.5 Use-Case Coverage & Architecture Fit

**Verdict: the architecture supports every use case; none is end-to-end runnable yet.** The control-flow skeleton (FSM, lanes, mode graph, PipeWire data plane) is real; the *DSP muscle* (PBFDAF/MVDR/SES/ASRC) and the *nervous-system wiring* (mq IPC, VAD-emit, KWD) are TODO. No **architectural** blocker was found — the gaps are implementation-completeness, not design flaws.

| Use case | Path (FSM edge) | Arch supports? | Runnable now? | Blocking gaps |
|---|---|---|---|---|
| **UC1 Wake → capture** | `SS_IDLE→SS_CAPTURE` | ✅ yes | ⛔ no | KWD stub, pre-roll unwired, **mq transport** |
| **UC2 Conversation** (capture→think→speak) | `SS_CAPTURE→SS_THINK→SS_SPEAK→SS_IDLE` | ✅ yes | ⛔ no | cloud connector stub, **mq transport** |
| **UC3 Barge-in** (interrupt TTS) | `SS_SPEAK→SS_BARGE_DUCK→SS_CAPTURE` | ✅ yes | ⛔ no | **VAD emitter missing**, DUCK_PLAYBACK TODO, **mq transport** |
| **UC4 Fault / reset** | `*→SS_FAULT` | ✅ yes | 🟡 partial | RESET_PIPELINE TODO, codec reset stub, **mq transport** |
| **UC5 Audio enhancement quality** | (data plane) | ✅ framework | ⛔ passthrough | all DSP algorithm bodies stub |

> Both §17.2 KPI integration tests (`barge_in_e2e`, `kwd_wake_e2e`) currently `GTEST_SKIP()` — they are placeholders, not coverage.

**Critical path to the first end-to-end turn** (dependency-ordered):
1. **POSIX mq transport** (`MsgBus` SendMsg/RecvMsg) — unblocks *all* inter-process flow; nothing works cross-process until this is real.
2. **VTS KWD + pre-roll wiring** (UC1) and **a VAD node that emits `BARGE_IN`** (UC3) — the two KPI paths.
3. **Cloud connector socket** (UC2 conversation).
4. **ABOX control verbs** (START_CAPTURE/DUCK/RESET) + **1 ms fader** (ducking, UC3/UC4 quality).
5. **DSP algorithm bodies** (AEC→beamform→SES→SRC) — audio quality (UC5); the framework already carries them.
6. **Converge the node model (D9):** route `PwStage` to the C `abox_node` vtable (and retire the C++ `dsp::Node` layer), or formally amend D9 to "C offline / C++ live." Today the live path contradicts D9.

**Design-level risks to watch** (not blockers, but decide deliberately):
- **D9 divergence** — two node implementations (C offline, C++ live) will drift until converged (item 6).
- **Buffer-pool semantics** — §18.2 caveat: the pool detects overruns but absorbs them only once the async pool-dispatch path is wired.
- **Reference delay-lock** — without the §4.3.2c cross-correlation lock + §5 drift PI, AEC will not converge on real hardware even after the PBFDAF kernel lands.

---


---

# Part III — abox ↔ PipeWire Architecture & Per-Mode Call Sequences

> **Folded in (2026-06-29) from the former `docs/abox_pipewire.md`** — how the C data plane
> (`abox_*`) runs live inside PipeWire (Model B) and the full per-block / per-mode call sequence,
> grounded in the implementation with file/function references. Complements Part I §13 and §16.5–16.6
> (block + call flows) and Part II §14 / §18 (deployment + as-built). **Section numbers below (§1–§7)
> are Part III-local.**
How the C audio data plane (`abox_*`) runs live inside PipeWire (Model B), and the full
call sequence for each use-case mode. Grounded in the implementation — file/function
references are given so this stays honest against the code.

Status legend:  `✅` built, live & tested · `⚠` framework live, DSP kernel is a passthrough stub · `⛔` not wired yet

---

## 1. The architecture (Model B — one filter hosts the whole C graph)

A single PipeWire filter node, **`hermes.abox`** (2 mic inputs `in_0/in_1`, 1 mono output
`out_0`), whose `on_process` drives the entire C engine. The DSP nodes never see PipeWire —
the bridge (`abox_main.cpp`) is the only `pw_*` boundary.

```
══════════ CONTROL PLANE (C++, non-RT) — MsgBus over POSIX mq ══════════
 SUPERVISOR ✅   VTS/KWD ⛔   LLM_CONN ⛔   CODEC ⛔   VIDEO ⛔   GUI_INTERFACE ✅(test)
      │ SET_MODE / SET_VOLUME / RESET_PIPELINE / DUCK_PLAYBACK            │
      └──────────────────────────┬───────────────────────────────────────┘
                                 ▼   /hermes.mod.2  (AUDIO_CORE inbox)
                   ┌─────────────────────────────┐
                   │ AudioCore (abox_main.cpp) ✅ │  EventMap: SET_MODE→set_mode ✅,
                   │  MsgBus recv thread         │  SET_VOLUME→set_gain ✅, RESET ⚠
                   └──────────────┬──────────────┘
                                  │ atomic mode/gain store (lock-free, read per block)
═════════════════════════════════│════ PIPEWIRE RT DOMAIN (data-loop, A76) ════
                                  ▼
   ┌───────────────────────────────────────────────────────────────────────┐
   │ PipeWire libpipewire-0.3 ✅   spa_io_position clock · in_0/in_1 · out_0  │
   │   Pw.cpp on_process(pos)  ──pw_filter_get_dsp_buffer──►  BlockFn         │
   └───────────────────────────────┬───────────────────────────────────────┘
                                    ▼   abox_block(user,in,chIn,out,chOut,n,pos)  [abox_main.cpp]
   ┌───────────────────────────────────────────────────────────────────────┐
   │ COORDINATOR  hermes_pipeline_process_tick()  ✅   [buffer_pipeline.c]    │
   │   async (default): bp_tick_async — COLLECT prior slot → INGEST new slot │
   │   Core-Proportional Buffer Pool: 1 frame slot per A76 worker core       │
   │     slot[i] state FREE→READY→DONE ; firewall=atomic ; overflow→drop     │
   │   vDMA-IN node (capture→slot) ✅   vDMA-OUT node (slot→out)×gain ✅       │
   └───────────────────────────────┬───────────────────────────────────────┘
                                    ▼   abox_frame (planar chan[c][], in place)
   ┌───────────────────────────────────────────────────────────────────────┐
   │ WORKER THREAD i (cpu5+i, prio 88)  bp_slot_worker  ✅                    │
   │   abox_graph_tick(graph, slot, mode)   mask = abox_active_mask(mode)     │
   │   ┌────────┐   ┌────────┐   ┌──────────┐   ┌────────┐                    │
   │   │ SRC ✅ │──►│ AEC ⚠  │──►│ BEAM ⚠   │──►│ SES ⚠  │ (each gated by mask)│
   │   │ resamp │2ch│ bypass │2ch│ bypass→1 │1ch│ bypass │                    │
   │   │ (1.0=  │   │ +ref   │   │ (chan0)  │   │        │                    │
   │   │  ident)│   │ pull   │   │          │   │        │                    │
   │   └────────┘   └───┬────┘   └──────────┘   └────────┘                    │
   │                    │ aligned far-end                                     │
   │              ┌─────▼────────────┐                                        │
   │              │ RefManager ✅     │  ring + VI-Sense + bulk delay          │
   │              │ (⛔ no xcorr lock)│                                        │
   │              └──────────────────┘                                        │
   └───────────────────────────────────────────────────────────────────────┘
       overrun (all slots busy) → drop ; underflow (slot not DONE) → abox_soft_mute ✅
```

**Key files:** `pipewire/Pw.cpp` (filter + `on_process`), `pipewire/abox_main.cpp` (bridge +
`AudioCore` control + node/vDMA wiring), `abox/buffer_pipeline.c` (coordinator + pool + async
workers), `abox/abox_graph.c` (mask-gated tick), `abox/abox_routing.c` (mode→mask), `abox/nodes/*.c`.

---

## 2. The two ticks (don't conflate them)

| | `hermes_pipeline_process_tick` | `abox_graph_tick` |
|--|--------------------------------|-------------------|
| Role | **coordinator** — pool firewall, ingest/egress, mode read | **DSP cascade** — walk nodes |
| Thread | PipeWire **RT** thread | **worker** thread (async) or caller (sync) |
| Cadence | once per 5 ms quantum (240 frames @ 48 kHz) | once per slot, per block |
| Touches PipeWire? | yes (in/out buffers) | never (only `abox_frame`) |

Async (default, `hermes_pipeline_start_async`): the RT tick **never runs DSP** — it only
COLLECTs the previous slot's result and INGESTs the new period, handing slots to workers.
~1-period latency, and a slow block degrades to one Soft-Mute instead of an ALSA Xrun.
`HERMES_SYNC=1` forces the inline path (graph runs on the RT thread).

---

## 3. Modes → active node mask (`abox_routing.c`)

`kGain[mode][elem] > 0` ⇒ that element is in `active_pipeline_mask`; the tick runs a node iff
its bit is set, else zero-copy SKIP (frame already carries the data, in place).

| Mode (`abox_mode`) | SRC | AEC | BEAM | SES | nodes that run |
|--------------------|:---:|:---:|:----:|:---:|----------------|
| `KEYWORD_LISTENING` (0, default) | – | – | – | – | **none** (idle/wake; pure passthrough) |
| `BARGE_IN_MUTING` (1) | ✓ | ✓ | ✓ | ✓ | SRC→AEC→BEAM→SES (TTS ducked) |
| `CONVERSATION` (2) | ✓ | ✓ | ✓ | ✓ | SRC→AEC→BEAM→SES (full duplex) |
| `SYSTEM_RESET` (3) | – | – | – | – | **none** (safe/muted) |

Set at runtime via `SET_MODE` CMsg → `hermes_pipeline_set_mode` (atomic, read per block), or
initially via the `HERMES_MODE` env (headless tests). `REF/CAP/TTS` are mask elements but not
graph nodes today (REF feeds AEC via the RefManager; CAP/TTS are gates, not yet nodes).

**Current node behavior** — every DSP kernel is a passthrough until implemented, so
`output buffer == input buffer` through the whole chain (see `test_loopback_bypass`):
- **SRC** ✅ real fractional resampler; at unity ratio (no drift) it's a bit-exact identity.
- **AEC** ⚠ pulls the aligned far-end reference (groundwork) then **bypasses** (PBFDAF + DTD TODO).
- **BEAM** ⚠ **bypass**: passes `chan[0]` unchanged, drops to mono (MVDR/GSC steering TODO).
- **SES** ⚠ **bypass** (spectral suppression / dereverb / AGC TODO).
- egress applies master **gain** (`SET_VOLUME`; skipped at unity).

---

## 4. Per-block call sequence (common to every mode)

```
PipeWire RT thread — every 5 ms (240 frames @ 48 kHz)
│
├─ Pw.cpp        on_process(io_position)
│                  pw_filter_get_dsp_buffer(in_0), (in_1), (out_0)
│                  BlockFn(user, in[2], 2, out[1], 1, 240, sample_pos)
│
├─ abox_main.cpp abox_block(...)  →  hermes_pipeline_process_tick(e, in,2, out,1, 240, pos)
│
└─ buffer_pipeline.c  bp_tick_async(...)            ← async path (default)
     (1) COLLECT  slot[out_idx]==DONE ?
                    ├ yes → bp_egress: vDMA-OUT slot→out_0, × gain ; slot→FREE ; processed++
                    └ no  → abox_soft_mute: zero-fill out_0 (underflow)
     (2) INGEST   slot[in_idx]==FREE ?
                    ├ yes → bp_ingest: vDMA-IN in_0/in_1 → slot ; capture mode atomic ;
                    │        slot→READY  ── wakes worker[in_idx]
                    └ no  → drops++ (all slots busy → overflow drop, protects cadence)

Worker thread i (cpu5+i)  bp_slot_worker
     slot[i]==READY ? → abox_graph_tick(graph, worker_buffers[i], slot_mode[i]) → slot[i]=DONE
                          └─ §5: which nodes run depends on the mode mask
```

`§5` below expands the `abox_graph_tick` step for each use case (the only part that differs
by mode). Everything above is identical regardless of mode.

---

## 5. Full call sequences per use-case scenario

Each shows the `abox_graph_tick` expansion — **every stage**, marked RUN or SKIP by the mask.

### 5.1 Playback / loopback — KEYWORD_LISTENING (0)  *(default; the `run_loopback.sh` case)*

Idle/wake: the engine is a clean conduit; no DSP runs.

```
abox_graph_tick(graph, slot, KEYWORD_LISTENING)   mask = 0x00
  stage SRC  (ABOX_ELEM_SRC )  mask&bit=0 → SKIP
  stage AEC  (ABOX_ELEM_AEC )  mask&bit=0 → SKIP
  stage BEAM (ABOX_ELEM_BEAM)  mask&bit=0 → SKIP
  stage SES  (ABOX_ELEM_SES )  mask&bit=0 → SKIP
  ran = 0   → slot frame unchanged (still 2ch)
egress: produced = min(2, out_channels=1) = 1 → copies slot.chan[0] → out_0, × gain
RESULT: out_0 == in_0 (the primary mic), bit-exact. Verified: SYS-04 / ABOX-11.
```

### 5.2 Conversation — CONVERSATION (2)  *(full duplex; all nodes run)*

```
control: SET_MODE(2) CMsg → AudioCore.onSetMode → hermes_pipeline_set_mode(CONVERSATION)
          (next block's INGEST captures mode=2 into the slot)

abox_graph_tick(graph, slot, CONVERSATION)   mask = SRC|AEC|REF|BEAM|SES|CAP|TTS
  stage SRC  RUN  src_process   : resample by ratio (1.0 ⇒ identity); carry phase/tail   [2ch→2ch]
  stage AEC  RUN  aec_process   : abox_ref_read_aligned(ref) → far-end; ramp mix; BYPASS  [2ch→2ch]
                                   (⚠ PBFDAF cancel = TODO, so output == input today)
  stage BEAM RUN  beam_process  : io->channels = 1, keep chan[0] (⚠ MVDR steer = TODO)    [2ch→1ch]
  stage SES  RUN  ses_process   : default bypass (⚠ spectral suppression = TODO)          [1ch→1ch]
  ran = 4
egress: produced = min(1,1)=1 → slot.chan[0] → out_0, × gain
RESULT: full chain executes; audio currently passes through (kernels are stubs).
```

### 5.3 Barge-in — BARGE_IN_MUTING (1)  *(user interrupts TTS)*

Same node set as CONVERSATION, but TTS is ducked. Today barge-in is **driven from the control plane**
(GUI/Supervisor) — the in-graph VAD emitter is ⛔ not built yet.

```
trigger: user speaks during TTS  (⛔ no VAD node emits AUDIO_CORE::evt::BARGE_IN yet;
         GUI "Barge-In" button / Supervisor stands in)
control: DUCK_PLAYBACK CMsg (URGENT) + SET_MODE(1) CMsg (URGENT) → AudioCore
          → hermes_pipeline_set_mode(BARGE_IN_MUTING)

abox_graph_tick(graph, slot, BARGE_IN_MUTING)   mask = SRC|AEC|REF|BEAM|SES|CAP  (TTS=0)
  stage SRC  RUN  src_process   : drift resample (identity at 1.0)                        [2ch→2ch]
  stage AEC  RUN  aec_process   : pull aligned reference; ramp toward cancel; BYPASS      [2ch→2ch]
                                   (AEC active here so the user's voice is echo-isolated — kernel TODO)
  stage BEAM RUN  beam_process  : → mono (chan[0])                                        [2ch→1ch]
  stage SES  RUN  ses_process   : bypass                                                  [1ch→1ch]
  ran = 4
egress: slot.chan[0] → out_0, × gain   (TTS path muted/ducked by the mode + DUCK_PLAYBACK)
RESULT: capture stays live & AEC-gated for the interrupting speech; playback ducks.
```

### 5.4 System reset — SYSTEM_RESET (3)

```
control: RESET_PIPELINE CMsg → AudioCore.onReset (⚠ handler stub) ; SET_MODE(3)
abox_graph_tick(graph, slot, SYSTEM_RESET)   mask = 0x00
  SRC/AEC/BEAM/SES → all SKIP   (ran = 0)
egress: slot.chan[0] → out_0 (or Soft-Mute on underflow) → safe/muted output
```

### 5.5 Overflow / underflow (any mode) — pool firewall

```
INGEST: all slots busy (worker behind) → drops++ , return 1  → period soft-dropped,
        ALSA cadence preserved (no Xrun); driver keeps ping-ponging.
COLLECT: slot[out_idx] not yet DONE (worker still cascading) → abox_soft_mute zero-fills
         out_0 for this period (§6.1). Next period collects it ~1 quantum late.
```

---

## 6. Control-plane entry points (what drives the modes)

`AudioCore` (in `abox_main.cpp`) subclasses `MsgBus`+`EventMap` on `/hermes.mod.2`:

| CMsg (Catalog `_AudioCore::cmd`) | Handler | Effect |
|----------------------------------|---------|--------|
| `SET_MODE` (int body) | `onSetMode` ✅ | `hermes_pipeline_set_mode` (atomic) |
| `SET_VOLUME` (float body) | `onSetVolume` ✅ | `hermes_pipeline_set_gain` (egress gain) |
| `RESET_PIPELINE` | `onReset` ⚠ | TODO §6.2 |
| `START_CAPTURE`/`DUCK_PLAYBACK`/`ARM_BARGE_IN`/… | — ⛔ | TODO finer controls |

For dev/test, **`hermes_gui_interface`** (ModuleId 7) turns browser actions into these CMsgs
over the bus (`scripts/run_gui.sh`). See `app/gui_interface/`.

---

## 7. Implementation status vs. the design

**Live & tested ✅** — Model B single filter; the C data plane is the live path (the old "D9"
C++/offline split is gone); async Core-Proportional buffer pool + per-slot workers + firewall;
vDMA ingress/egress nodes; mask-gated graph tick; mode/gain atomic control; RefManager ring +
VI-Sense; SRC resampler; Soft-Mute; master gain; MsgBus mq; GUI control bridge. Green in
`abox_selftest` (incl. `test_loopback_bypass`) + the PipeWire loopback (SYS-04), cross-built for RK3588.

**Framework live, kernel TODO ⚠** — AEC (PBFDAF + DTD; reference pull is real), BEAM (MVDR/GSC;
bypass downmix today), SES (spectral suppression). All are exact passthroughs until implemented.

**Not wired ⛔** — VAD / in-graph `BARGE_IN` emitter (barge-in is control-driven for now);
pre-roll ring (VTS→ABOX); §5 1 ms drift PI loop feeding `abox_src_set_ratio`; RefManager
cross-correlation delay-lock; llm-connector/KWD/codec process bodies; `RESET_PIPELINE` body.

> See [SVVR.md](./SVVR.md) for test-case IDs and [../VALIDATION.md](../VALIDATION.md)
> for how to run each path (incl. the audio loopback that exercises this whole sequence).
