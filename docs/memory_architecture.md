# Hermes — Memory Subsystem Design

**Status:** Draft v0.1 · 2026-06-29 · *for review* · companion to `ARCHITECTURE.md` §17
**Scope:** the listener/story memory subsystem — retrieval, management, storage, call flow.

---

## 1. Purpose & Scope

Specify how Hermes **remembers the listener and the story across sessions** and **recalls the
relevant bit per turn** — local-first, private, on a small board. Curated knowledge
(guardrails/persona/characters/book lore) is covered in `ARCHITECTURE.md` §17; this document
focuses on the **dynamic** memory (what the device learns from interaction).

Default design: **OKF markdown + local retrieval**, with **mem0 as an optional engine** behind
the same facade.

## 2. Functional Specification (FR)

| ID | Requirement |
|----|-------------|
| FR-M-1 | Capture each interactive turn (listener utterance + system response) to an episodic log. |
| FR-M-2 | Persist durable facts about the listener (name, age band, level, preferences, favorites). |
| FR-M-3 | Persist durable story facts and progress (book, position, what they liked/asked). |
| FR-M-4 | On a turn, **recall** the top-k facts relevant to the current utterance + active book. |
| FR-M-5 | **Consolidate** episodic logs into durable facts at idle (ADD / UPDATE / DELETE). |
| FR-M-6 | Scope memory by listener (and optionally per character). |
| FR-M-7 | Export a human-readable snapshot for parent review; support **erase** (per fact / all). |
| FR-M-8 | Operate with no network (recall + capture fully local). |
| FR-M-9 | Degrade gracefully: if the retrieval backend is unavailable, recall returns empty and the device still functions. |

## 3. Non-Functional Specification (NFR)

| ID | Attribute | Target / Constraint |
|----|-----------|---------------------|
| NFR-M-1 | Recall latency | ≤ 150 ms p95 on-device (role-load + local search), never on the audio RT path |
| NFR-M-2 | Capture latency | ≤ 5 ms (append-only write), non-blocking to the IPC loop |
| NFR-M-3 | Consolidation | off-turn only (idle/"sleep"); seconds-to-minutes acceptable; cloud-offloadable |
| NFR-M-4 | Footprint | recall path adds < ~150 MB RAM (embeddings optional); **no vector server required** |
| NFR-M-5 | Durability | survive power loss (atomic file writes; fsync on consolidation) |
| NFR-M-6 | Privacy | on-device, encrypted at rest; no cloud egress without verifiable parental consent |
| NFR-M-7 | Auditability | all memory is human-readable OKF; parent can view/erase |
| NFR-M-8 | Bounded growth | episodic pruned after consolidation; semantic capped (§6.4) |
| NFR-M-9 | Portability | storage is plain files (git/tar/fs); no proprietary DB required |
| NFR-M-10 | Swappability | backend (local search ↔ mem0 ↔ cloud) changes behind the facade, no caller change |

## 4. Use Cases & Subsystem Integration (abox · PipeWire · LLM)

Memory is not standalone — it sits inside the audio loop. This section traces it through the
real Hermes stack.

### 4.0 Where memory touches the audio + LLM chain
```
                       ┌──────────────── RT / DATA PLANE (PipeWire, 5 ms) ────────────────┐
 mic ──in_0/in_1──►  hermes.abox filter:  SRC ─► AEC ─► BEAM ─► SES ─► out_0 ──► speaker   │
                                            ▲ far-end ref                                  │
                            RefManager ◄──── TTS the device just played (echo reference)    │
                       └──────────────┬───────────────────────────────────────┬───────────┘
   clean mono (echo-free) ────────────┘ (PipeWire link)        TTS audio ──────┘ (PipeWire link)
                       │                                                        ▲
══════════════════════│════════ CONTROL PLANE (off the RT path) ═══════════════│════════════
                       ▼                                                        │
   llm_connector: STT ─► [recall] ─► prompt ─► route() local|cloud ─► gate ─► TTS
                       │ ▲                                                       │
                       │ │ Memory.recall(user, transcript)   Memory.remember(user, turn)
                       ▼ │                                                       ▼
   story_agent: position/casting ◄── _Story ──► supervisor (modes/FSM)     episodic log
                       │                                                        │ (idle)
                       └────────────────────── MEM_ROOT (OKF) ◄── Consolidator ─┘
```
**Three integration facts that make this non-generic:**
1. **STT consumes abox's *clean* output, not the raw mic.** AEC (with the TTS as far-end
   reference via `RefManager`) + beamform produce echo-free mono → transcripts (hence memory) are
   clean even while the device is talking. Memory quality depends on the abox DSP path.
2. **The TTS the device plays is itself the AEC reference.** llm_connector plays TTS into
   PipeWire; abox taps it as the far-end so the child talking *over* narration doesn't poison the
   transcript. This closes the PipeWire↔abox↔memory loop.
3. **Recall/remember run on the control plane**, never inside `process_tick` (the 5 ms RT loop).
   During a barge-in the narration is *ducked* (`BARGE_IN_MUTING`), so recall+LLM latency overlaps
   silence — no audio underrun, no Soft-Mute.

### 4.1 UC — Interactive barge-in Q&A (the full trace)
Child interrupts the story to ask a question; memory shapes the answer; narration resumes.

```
PHASE 1 — NARRATING (abox mode CONVERSATION)
  story_agent ──_Llm::cmd::PLAY_SEGMENT(idx=12)──► llm_connector
  llm_connector: TTS("Sherlock", "analytical", line) ──PipeWire──► hermes.abox out_0 ──► speaker
                 (same TTS ──► RefManager far-end, for AEC)

PHASE 2 — BARGE-IN DETECT & DUCK  (≤ ~1 quantum; control on URGENT lane)
  mic ──► abox: SRC→AEC(removes narration echo)→BEAM(2→1)→SES ──► clean mono
  VAD on clean mono  (⛔ planned)  ──_AudioCore::evt::BARGE_IN (URGENT)──► supervisor
  supervisor ──SET_MODE(BARGE_IN_MUTING)+DUCK_PLAYBACK──► audio_core   (TTS element gated → duck)
  supervisor ──_Story::cmd::PAUSE──► story_agent   (hold position at idx 12)

PHASE 3 — CAPTURE QUESTION
  clean mono ──PipeWire link──► llm_connector STT ──► transcript "Why did Pip hide?"
  llm_connector ──_Llm::evt::STT_FINAL(text)──► story_agent

PHASE 4 — RECALL  (control plane; overlaps the duck silence)
  story_agent.onUserSpeech(text):
     facts = Memory::recall(user="mia", query="Why did Pip hide?", k=3)
        → role-load: brain/characters/pip.md (active), persona, guardrails
        → structured+keyword over MEM_ROOT: BookFact(ch.3 events), MemoryFact("Mia asked about Pip before")
        → ranked top-k                                            [NFR-M-1 ≤150 ms]

PHASE 5 — GENERATE (memory-conditioned) + SAFETY
  llm_connector: prompt = guardrails (+ sensitivity facts: "no scary parts")
                        + persona + pip.md + recalled facts + working memory(last N turns)
                 route()  → simple → LOCAL LLM      → answer
                 OUTPUT GATE (guardrails layer 2)   → ok | safe-fallback
  answer ──► TTS("Pip","gentle") ──PipeWire──► abox out_0 ──► speaker

PHASE 6 — REMEMBER + RESUME
  story_agent: Memory::remember("mia", turn{q,a})  → append episodic (no LLM, ms)
  supervisor ──SET_MODE(CONVERSATION)+_Story::cmd::RESUME──► (replay idx 12)
```
Note how the answer is shaped by **three memory inputs** (Pip character OKF, story BookFacts, the
listener's prior interest) and constrained by a **sensitivity MemoryFact** fed into guardrails.

### 4.2 UC — Wake & personalized greeting (cold start)
```
power on → supervisor Idle (abox KEYWORD_LISTENING: DSP bypassed)
VTS wake / button ──WAKE_CONFIRMED──► supervisor ──► story_agent
Memory::recall("mia","") → ListenerProfile(name=Mia, favorite=Pip), BookFact(progress=ch.3)
llm_connector: greeting from facts ──TTS──► abox ──► speaker:
   "Hi Mia! Want more of Pip the Fox — we stopped where he found the window."
```
Memory → LLM → TTS → PipeWire, before any book even plays.

### 4.3 UC — Narration personalized by memory (casting & emotion caps)
```
Memory facts: ListenerProfile(pace=slow), MemoryFact(sensitivity="no scary parts")
story_agent loads book OKF, resumes at BookFact(progress); casting reads characters/*.md
   → emotion mapping CLAMPED by the sensitivity fact (caps "scary"/"loud" intensity)
PLAY_SEGMENT loop ──► llm_connector TTS(voice+capped emotion) ──► abox(CONVERSATION) ──► speaker
```
A stored fact changes both the **LLM/casting** and the **audible emotion** — memory ↔ TTS ↔ abox.

### 4.4 UC — Preference learned now, applied next session (the write→consolidate→read loop)
```
SESSION A:  child: "I don't like the scary parts."  → Memory::remember → episodic
SLEEP:      supervisor ──CONSOLIDATE──► Consolidator: LLM(episodic+MEMORY.md)
               → UPDATE/ADD MemoryFact{type, tags:[sensitivity], confidence:0.9} in OKF
               → refresh MEMORY.md, reindex, prune episodic
SESSION B:  recall surfaces the sensitivity fact → injected into guardrails + casting (UC 4.3)
               → narration is gentler, answers avoid scary framing
```

### 4.5 Other use cases (summary)
| ID | Use case | Memory ops | Touches |
|----|----------|-----------|---------|
| UC-M-1 | "What's my name?" | recall(ListenerProfile) | LLM, TTS |
| UC-M-3 | Resume where stopped | recall(BookFact progress) | story_agent, TTS |
| UC-M-6 | Parent review/erase | export + erase | OKF visualizer |
| UC-M-7 | Offline bedtime session | local recall + capture | no cloud |

### 4.6 Latency budget (RT plane vs control plane)
| Stage | Plane | Budget | Notes |
|-------|-------|--------|-------|
| mic → clean mono (AEC/beamform) | RT (abox) | per 5 ms quantum | never blocked by memory |
| barge-in detect → duck | control, URGENT | ≤ 12 ms (⛔ needs VAD) | gates TTS, holds position |
| STT | control | streaming | on clean mono |
| **recall** | control | **≤ 150 ms p95** | overlaps duck silence (no underrun) |
| LLM + gate | control | local 100s ms–s | route()/cloud as needed |
| remember (capture) | control | ≤ 5 ms | append-only; heavy extraction deferred to idle |
| consolidation | control, idle | seconds–min | off-turn; cloud-offloadable |

## 5. Where Memory Is Maintained (storage map)

```
<MEM_ROOT>/                         on-device, encrypted (e.g. /var/lib/hermes/brain, parent-erasable)
├── MEMORY.md                       OKF index — one line per durable fact (cheap recall surface)
├── semantic/                       durable facts as OKF MemoryFact docs (one fact per file)
│   ├── listener-profile.md
│   ├── book-<slug>.md
│   └── fact-<id>.md
├── sessions/                       episodic raw logs, one per session (pruned after consolidation)
│   └── 2026-06-29T19-30.md
└── .index/                         derived retrieval index (rebuildable): FTS db and/or embeddings
CURATED (read-only at runtime):  brain/{guardrails,persona}.md, brain/characters/*.md (OKF)
OPTIONAL: mem0 sidecar store (Chroma) — only if mem0 backend selected
CLOUD (opt-in): encrypted backup of <MEM_ROOT>; parent dashboard; offloaded consolidation
```
`MEM_ROOT` is the single source of truth; `.index/` is derived and can be rebuilt from the OKF
files. Curated OKF is separate (versioned with the product); dynamic memory is per-device.

### 5.1 OKF MemoryFact schema
```yaml
---
type: MemoryFact            # required
title: Favorite character
description: durable listener preference
tags: [preference, character]
user: child-01              # scope
agent: pip                  # optional per-character scope
confidence: 0.9
timestamp: 2026-06-29T19:31:00Z
source: sessions/2026-06-29T19-30.md
---
The listener's favorite character is Pip the Fox; asks for him by name. See [[listener-profile]].
```

## 6. Memory Management (lifecycle)

```
 capture ──► episodic ──┐
                        ▼  (idle / "good night")
                   CONSOLIDATE (LLM) ── ADD / UPDATE / DELETE ──► semantic OKF + MEMORY.md
                        │                                          │
                        └── prune consolidated episodic ◄──────────┘ reindex .index/
```

### 6.1 Capture
Append turn to `sessions/<ts>.md` (append-only, atomic). No LLM, no blocking. Working memory
(last N turns) is held in RAM by `story_agent`/`llm_connector` for the live prompt.

### 6.2 Consolidation (the only LLM-heavy step)
Triggered by `supervisor` at idle / session end / "good night". An LLM reads new episodic turns +
existing `MEMORY.md` and emits a change set: **ADD** (new fact), **UPDATE** (supersede outdated),
**DELETE** (contradicted/obsolete). Applied as OKF file writes; `MEMORY.md` refreshed; `.index/`
updated for changed docs. Idempotent and resumable; can run on-device or be offloaded to cloud.

### 6.3 Retention & privacy
Episodic pruned after successful consolidation (configurable grace window). Parent erase removes
the OKF file(s) + index entries (and cloud backup if enabled). Confidence + timestamp drive
UPDATE/DELETE and stale-fact decay.

### 6.4 Bounded growth (eviction)
Semantic facts capped per scope (e.g. N per listener/book). On overflow, evict by lowest
`confidence` × recency. `MEMORY.md` stays small (it's the per-turn recall surface).

## 7. Memory Retrieval

### 7.1 Query model
`recall(user_id, query, k) → [fact]`. Inputs: the listener utterance (or current narration
context) + active book/character scope. Output: ranked durable facts for the prompt.

### 7.2 Tiers (compose; lightest first)
| Tier | Mechanism | Always on? |
|------|-----------|-----------|
| Role-load | include guardrails/persona + active book characters (curated OKF) | yes |
| Structured | filter `MEMORY.md`/frontmatter by `type`/`tags`/`user`/`book`; follow links | yes |
| Keyword | FTS (SQLite FTS5 / ripgrep) over semantic docs in scope | yes |
| Semantic | embed query → cosine over `.index/` embeddings (local MiniLM) | optional |

### 7.3 Ranking & assembly
Merge tier hits; score = lexical/semantic similarity × `confidence` × recency; dedupe by file;
take top-k within a **token budget**; format as compact bullet facts injected into the system
prompt. (No per-turn LLM; no vector server.)

## 8. Block Diagram

```
            ┌───────────────────────── story_agent / llm_connector ─────────────────────────┐
            │                                                                                │
 turn ─────►│  working memory (RAM, last N turns)                                            │
            │        │ recall(user,query)                 │ remember(user,turn)              │
            │        ▼                                     ▼                                 │
            │  ┌──────────────┐                     ┌──────────────┐                         │
            │  │ Memory facade│  recall/remember/export                                      │
            │  └──────┬───────┘                     └──────┬───────┘                         │
            └─────────│────────────────────────────────────│─────────────────────────────────┘
                      ▼ (selected backend)                  ▼
        ┌─────────────────────────────┐          ┌─────────────────────────────┐
        │ Retrieval (local, default)  │          │ Capture                      │
        │  role-load + structured     │          │  append episodic sessions/*  │
        │  + FTS + (opt) embeddings   │          └──────────────┬──────────────┘
        │  over OKF in MEM_ROOT       │                         │
        └─────────────┬───────────────┘                         │ (idle)
                      │ reads                                    ▼
        ┌─────────────▼───────────────────────────────────────────────────────┐
        │  MEM_ROOT (OKF, on-device, encrypted)                                 │
        │   MEMORY.md · semantic/*.md · sessions/*.md · .index/                 │
        └─────────────▲───────────────────────────────────────────────────────┘
                      │ writes ADD/UPDATE/DELETE
        ┌─────────────┴───────────────┐     optional, swappable
        │ Consolidator (idle LLM job) │     ┌───────────────────────────┐
        └─────────────────────────────┘     │ mem0 sidecar (HTTP)        │ ← alt backend for
                                             │ Chroma + embeddings + LLM  │   retrieval+extract
                                             └───────────────────────────┘
```

## 9. High-Level Sequence

```
RECALL (per turn)          REMEMBER (per turn)         CONSOLIDATE (idle)
 utterance                  turn complete               supervisor: SLEEP
   │ recall()                │ remember()                 │ run consolidation
   ▼                         ▼                            ▼
 role-load + local search   append episodic log         LLM(episodic + MEMORY.md)
   │                         │ (no LLM, ms)               │  → ADD/UPDATE/DELETE
   ▼                         ▼                            ▼
 top-k facts → prompt       done                        write OKF + reindex + prune
```

## 10. Low-Level Sequence & Call Flow

### 10.1 Recall (local OKF backend — default)
```
llm_connector.handleTurn(transcript)
 └─ Memory::recall(user_id="child", query=transcript, k=3)
     ├─ OkfStore::roleLoad(active_book) ............ read brain/guardrails.md, persona.md, characters/*
     ├─ OkfStore::structured(filter{user,book,type:MemoryFact|BookFact})  ... scan MEMORY.md
     ├─ FtsIndex::search(query, scope) ............. SQLite FTS5 over semantic/*.md
     ├─ [opt] VecIndex::search(embed(query), scope)  cosine over .index/ embeddings
     ├─ rank(merge) → top-k within token budget
     └─ return facts  ──► prompt builder ──► route()/gate ──► TTS
```
*Optional mem0 backend variant:* `Memory::recall → http_post_json(127.0.0.1:7070,"/search",{user,query,top_k})
→ server.py do_POST(/search) → mem0.search(...) → {"facts":[...]}` (the C++ HTTP client +
sidecar already implemented; see `app/story_agent/http_client.hpp`, `services/memory/server.py`).

### 10.2 Remember (capture)
```
story_agent.onTurnComplete(turn)
 └─ Memory::remember(user_id, turn)
     └─ EpisodicLog::append(sessions/<ts>.md, turn)   // atomic append; returns immediately
     // local backend: NO network, NO LLM on the turn
     // mem0 backend: enqueue → worker → POST /add (LLM extraction, off the turn)
```

### 10.3 Consolidation (idle)
```
supervisor.enterSleep()
 └─ SendMsg(STORY_AGENT, _Story::cmd::CONSOLIDATE)         // (new cmd, to add)
     └─ Consolidator::run(user_id)            // worker thread
         ├─ turns = EpisodicLog::since(last_consolidated)
         ├─ change = LLM(prompt(turns, MEMORY.md))   → [{op:ADD|UPDATE|DELETE, fact}]
         ├─ for each change: OkfStore::apply(change)         // write/update/delete semantic/*.md
         ├─ OkfStore::reindex(changed)                        // refresh MEMORY.md + .index/
         └─ EpisodicLog::prune(consolidated)                  // bound growth
```

## 11. Interfaces (contracts)

```cpp
// Memory facade (story_agent) — backend-agnostic
std::string recall(user_id, query, k=3);   // → facts (compact text / JSON {"facts":[...]})
void        remember(user_id, turn);        // capture (append); heavy work deferred
void        exportMd(user_id);              // parent-readable snapshot (already OKF)
// Consolidation: Consolidator::run(user_id)  (idle worker; supervisor-triggered)
```
Backend selection by config: `local` (OKF search, default) | `mem0` (sidecar) | `cloud`.
HTTP backend API (if `mem0`/sidecar): `POST /search`, `POST /add`, `GET /health`
(see `services/memory/README.md`).

## 12. Limitations

1. **Local recall is lexical/embedding similarity** — no multi-hop reasoning over facts; complex
   "connect A and B" recall needs the LLM (or mem0's graph) — deferred.
2. **Consolidation depends on an LLM** — quality of ADD/UPDATE/DELETE ≈ the local model; weaker
   on-device than cloud. Mitigate: offload consolidation to cloud when online/consented.
3. **On-device semantic tier** adds an embedding model (~80 MB) — optional; keyword+structured
   work without it but miss paraphrase.
4. **No cross-device memory** without the opt-in cloud sync.
5. **Eviction is heuristic** (confidence×recency) — may drop a rarely-used but important fact;
   parent-pinned facts (future) would address this.
6. **Single-writer** assumption on `MEM_ROOT` (the consolidator) — no concurrent multi-writer.
7. **Barge-in recall** shares the interactive-turn path; its latency is bounded by NFR-M-1 only if
   the semantic tier stays in-memory.

## 13. Configuration

| Var | Default | Meaning |
|-----|---------|---------|
| `HERMES_MEM_BACKEND` | `local` | `local` (OKF search) \| `mem0` \| `cloud` |
| `MEM_ROOT` | `/var/lib/hermes/brain` | on-device memory store (encrypted) |
| `HERMES_MEM_SEMANTIC` | `off` | enable local embedding tier |
| `HERMES_MEM_HOST/PORT` | `127.0.0.1`/`7070` | sidecar (mem0 backend only) |
| `HERMES_MEM_CLOUD_CONSOLIDATE` | `off` | offload consolidation when online + consented |

## 14. Status (vs code)
✅ `Memory` facade + dependency-free HTTP client + mem0 sidecar (`services/memory/`), graceful
degradation, ping self-check. ⚠ Default backend is currently the sidecar; **local OKF retrieval
(`OkfStore`/`FtsIndex`) + `Consolidator` are planned (this spec)**. ⛔ semantic tier, eviction,
cloud sync, parent erase API.
