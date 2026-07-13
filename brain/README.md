# brain/ — the LLM's mind (curated knowledge as markdown)

The device's persona, safety rules, character casting, and consolidated memory live here as
**plain markdown (OKF) the LLM reads** — readable, editable, parent-auditable, version-
controllable, and model-agnostic (same files steer the local and cloud LLM paths). "Readable
files over opaque state."

> md is the **human/edit** layer. At runtime `llm_connector` **selects and compiles** what to
> inject (token budget) — it never dumps every file into every prompt.

## Layout
```
brain/
├── guardrails.md              # safety/behavior rules → system prompt (gate layer 1 of 2)
├── persona.md                 # who the device is (tone, role, narrator voice)   [TODO]
├── characters/                # per-character voice + emotion profiles (book casting)
│   └── example-character.md
└── memory/                    # tiered long-term memory (on-device, private)
    ├── MEMORY.md              # index: one line per durable fact (recall surface)
    ├── semantic/              # consolidated durable facts (one OKF MemoryFact per file)
    │   └── listener-profile.md
    └── sessions/              # episodic raw logs (pruned on consolidation)
```

## Design — see the canonical docs (don't duplicate here)
- **Memory model, retrieval, consolidation, lifecycle:** `docs/memory_architecture.md`
- **Architecture + knowledge/memory subsystem, OKF model, guardrail gate:** `docs/ARCHITECTURE.md` (§17–18)

In short: curated knowledge is **loaded by role** (guardrails/persona/active characters always)
or **searched locally** (OKF); dynamic facts are **written by the idle consolidation job** into
`memory/semantic/` as OKF. Per-device memory content stays on-device (gitignore the real
`memory/sessions/` + `memory/semantic/`); templates/guardrails/persona/characters are tracked.
