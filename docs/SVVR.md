# Hermes — Software Verification & Validation Report (SVVR)

**System under test (SUT):** Hermes Embedded AudioBox — audio DSP framework + control plane
**Document type:** Software Verification & Validation Report (IEEE 1012 / 829-aligned, pragmatic)
**Report version:** 2.0 · **Issued:** 2026-06-29 · **Status:** Draft for review
**SUT baseline:** `git 19450a8` (branch `main`, 2026-06-29) — *working tree carries uncommitted docs only; no source delta vs. baseline*
**Verification owner:** Engineering (jhsieh) · **Approver:** _pending_ · **QA witness:** _pending_

> **Scope of this report.** It records the verification (did we build it right?) and validation
> (did we build the right thing?) status of the SUT against the requirements in
> `ARCHITECTURE.md` Part I (§6 FR, §7 NFR). It is deliberately **honest about non-coverage**: the
> SUT is at *framework* maturity — the control-flow skeleton and the audio-transport spine are
> verified; the DSP algorithm kernels and the cross-process end-to-end use cases are **not yet
> validated** (stubs / skipped placeholders). See §8, §10, §12.

---

## Revision history
| Ver | Date | Author | Change |
|-----|------|--------|--------|
| 1.0 | 2026-06-28 | jhsieh | Initial test report (unit + system tables). |
| 2.0 | 2026-06-29 | jhsieh | Rewritten as an SVVR: added document control, environment baseline, methodology, results taxonomy, **requirements traceability matrix**, anomaly register, residual-risk and qualified release recommendation. **Corrected two reporting defects** (see §12 A-1, A-2): the two `*_e2e` cases are `GTEST_SKIP` placeholders (were shown as "Pass"); the "9/9 CTest" figure was stale (actual: 5 CTest cases). |

---

## 1. Purpose & Scope
**Purpose.** Provide an auditable statement of what has been tested, how, with what result, and
what remains unvalidated, so that release/iteration decisions are made on evidence.

**In scope.** Unit verification of the C data-plane engine and C++ IPC contract; integration
placeholders; system/E2E build-and-run checks; requirements traceability; coverage assessment.

**Out of scope.** DSP algorithmic quality benchmarking (ERLE, MOS, WER), on-device acoustic
acceptance, long-duration soak/thermal, security penetration testing, power measurement. These are
called out as residual risk (§13), not covered here.

## 2. Reference documents
| Ref | Document | Use |
|-----|----------|-----|
| R1 | `ARCHITECTURE.md` Part I §6/§7 | Requirements baseline (FR-1…FR-11, NFR-1…NFR-10) |
| R2 | `ARCHITECTURE.md` Part II §18 | As-built realization & implementation-gap audit |
| R3 | `ARCHITECTURE.md` Part III | Per-mode call sequences (basis for SYS-04 expectations) |
| R4 | `VALIDATION.md` | Validation *procedure* + exact commands + pass criteria |
| R5 | `BUILD.md` | Build/toolchain details |
| R6 | Source: `test/`, `app/audio_core/abox/abox_selftest.c`, `*/CMakeLists.txt` | Test definitions (evidence) |

## 3. Result taxonomy (definitions)
| Verdict | Meaning |
|---------|---------|
| **PASS** | Executed; all assertions held. |
| **FAIL** | Executed; an assertion failed. |
| **SKIPPED** | Test exists but asserts nothing this baseline (`GTEST_SKIP` placeholder) → **no coverage**. |
| **BLOCKED** | Cannot execute in available environment (e.g. requires RK3588 hardware). |
| **NOT COVERED** | No test exists for the requirement at this baseline. |

> **CTest vs. assertion granularity.** A `GTEST_SKIP` binary still exits 0, so CTest reports it
> *Passed*. This report classifies such cases as **SKIPPED** to avoid the false-confidence of a
> raw "5/5 passed" headline.

## 4. Test environment & configuration baseline
| Item | Value |
|------|-------|
| Host | Apple Silicon (arm64), macOS; Docker 29.x |
| Build container | `ubuntu:24.04`, `--platform=linux/arm64` (native on M-series; **no QEMU** for native/test) |
| Cross target | RK3588 aarch64 via `cmake/rk3588.toolchain.cmake` (`-mcpu=cortex-a76.cortex-a55`) — *compile-only* |
| Language std | C11 (data plane, `_Atomic`), C++17 (control plane) |
| Test frameworks | C `assert()` self-test (`abox_selftest`); GoogleTest (`GTest::gtest_main`) for C++ |
| Test runner | CTest; `./scripts/build.sh test` |
| Instrumentation | **None wired** — no `-fsanitize` (ASan/UBSan/TSan), no `--coverage`/gcov/lcov (see A-3, §12) |
| Last executed run of record | 2026-06-28 (data-plane + IPC suite); see §6/§14 |

## 5. Test methodology & levels
- **Unit (verification).** Deterministic, environment-free. C engine via assertion self-test
  (aborts on first failed invariant); C++ IPC via GoogleTest. Entry: green build. Exit: all
  assertions hold.
- **Integration.** Cross-module flows over the real POSIX-mq bus. **Status: placeholder only**
  (§8) — the harness to stand up SUPERVISOR + fakes is not yet written.
- **System / E2E (validation).** Script-driven against a live PipeWire graph and/or the device;
  pass criteria per R4. Environment-dependent; some BLOCKED without hardware.
- **Traceability.** Each FR/NFR mapped to its evidencing test(s) in §10.

---

## 6. Execution summary (headline)
**CTest suite — 5 registered cases** (`abox_selftest`, `test_eventmap`, `test_msgbus`,
`barge_in_e2e`, `kwd_wake_e2e`). Last executed run 2026-06-28, 0.30 s wall.

| Verdict | Count | Cases |
|---------|:---:|-------|
| **PASS (real coverage)** | **3** | `abox_selftest` (11 sub-cases), `test_eventmap`, `test_msgbus` |
| **SKIPPED (placeholder, no coverage)** | **2** | `barge_in_e2e`, `kwd_wake_e2e` |
| FAIL | 0 | — |

**System/E2E — 5 procedures (R4):** 4 executed PASS on host (SYS-01..04), 1 BLOCKED on hardware
(SYS-05). **Unit assertions exercised:** 11 `abox_selftest` sub-cases + EventMap dispatch + MsgBus
delivery/ordering/absent-peer.

> **Headline (honest):** the **data-plane engine and IPC contract are verified**; the **two KPI
> integration paths are unverified** (skipped); **DSP algorithm correctness and end-to-end use
> cases are unvalidated** (stubs). This is a framework-maturity result, not a release-candidate one.

---

## 7. Unit-level results

### 7.1 `abox_selftest` (C data-plane engine) — PASS · 11 sub-cases
One binary; each sub-case is an `assert`-guarded invariant run in sequence from `main()`
(`abox_selftest.c:367`). A failure aborts the binary → CTest FAIL. Evidence: `file:line`.

| TC ID | Sub-case (`abox_selftest.c`) | Asserts | Verdict |
|-------|------------------------------|---------|:---:|
| ABOX-01 | `test_routing_mask` :20 | per-mode `active_pipeline_mask` (idle=0x00, conversation=all, barge keeps AEC & drops TTS, reset=0x00) | PASS |
| ABOX-02 | `test_vdma` :329 | vDMA ingress/egress copy correctness (capture→slot, slot→out) | PASS |
| ABOX-03 | `test_graph_mask_gating` :36 | graph tick runs/SKIPs each stage by mode mask (zero-copy skip) | PASS |
| ABOX-04 | `test_reference_manager` :55 | AEC ref delay ring: zero-delay identity, integer shift, VI-Sense tap | PASS |
| ABOX-05 | `test_worker_pool` :84 | self-claim pool runs every slice exactly once (no double/zero claim) | PASS |
| ABOX-06 | `test_param_store` :98 | lock-free double-buffer publish/swap (no torn read) | PASS |
| ABOX-07 | `test_buffer_pipeline` :111 | Core-Proportional pool: ingest→tick→egress, overrun **soft-drop**, core rotation | PASS |
| ABOX-08 | `test_src_node` :151 | SRC: unity identity + fractional-resample interpolation, bounded / no-NaN | PASS |
| ABOX-09 | `test_playback_pipeline` :185 | full SRC→AEC→BEAM→SES sync path, bounded output, zero drops | PASS |
| ABOX-10 | `test_async_pipeline` :234 | per-slot worker threads produce real output across periods | PASS |
| **ABOX-11** ⭐ | `test_loopback_bypass` :282 | **full-graph loopback bit-exact `out0 == in0`; BEAM keeps chan[0] (not L+R avg); zero drops** | PASS |

⭐ **ABOX-11** is the unit twin of SYS-04. It proves the transport+graph path is **lossless** and
that every (currently stub) node passes audio through unchanged. **Caveat:** this assertion is only
valid *while* the DSP kernels are passthrough — a real AEC/BEAM/SES kernel will change the signal
and **must** be accompanied by an updated expectation (tracked, §13 RR-2).

### 7.2 `test_eventmap` (C++) — PASS
Verifies `EventMap<T>` dispatch: `id → bound member handler`, and the unhandled-id path
(`Execute` returns 0). Evidence: `test/unit/common/test_eventmap.cpp`.

### 7.3 `test_msgbus` (C++) — PASS
Verifies the POSIX-mq `MsgBus` contract: real `mq_open/mq_send/mq_timedreceive` delivery, priority
**lane ordering** (URGENT before NORMAL before DEFERRED), and **absent-peer** behaviour (send to a
non-listening module degrades, does not block/crash → evidences NFR-10 in part). Evidence:
`test/unit/common/test_msgbus.cpp`.

---

## 8. Integration-level results — **SKIPPED (no coverage)**
Both KPI-path integration tests are **source-level `GTEST_SKIP` placeholders** this baseline. They
register and "pass" at CTest granularity but assert nothing.

| TC ID | Case | State | Evidence | Blocking work |
|-------|------|:---:|----------|---------------|
| INT-01 | `barge_in_e2e` :9 (`FsmReachesBargeDuckOnBargeIn`) | SKIPPED | `GTEST_SKIP "TODO: wire SUPERVISOR + fake endpoints over the mq transport"` | mq-backed FSM harness |
| INT-02 | `kwd_wake_e2e` :9 (`FsmStartsTurnOnWakeConfirmed`) | SKIPPED | `GTEST_SKIP "TODO: wire SUPERVISOR + fake VTS/pre-roll ring"` | VTS fake + pre-roll ring |

> These are the **two system-KPI paths** (barge-in, keyword wake, R2 §17.2). Their non-coverage is
> the single largest validation gap (§13 RR-1).

---

## 9. System / End-to-End results
Script-driven; pass criteria per R4 (`VALIDATION.md`).

| TC ID | Procedure | Command | Expected (pass criterion) | Verdict |
|-------|-----------|---------|---------------------------|:---:|
| SYS-01 | Live PipeWire smoke | `./scripts/build.sh run` | engine connects + runs RT loop, no crash (exit 124→0) | PASS |
| SYS-02 | RK3588 cross-build | `./scripts/build.sh cross` | aarch64 ELFs emitted | PASS |
| SYS-03 | Web control console | `./scripts/run_gui.sh` + `curl` | UI action → control `CMsg` on the bus (`POST /api/cmd → [sent]`) | PASS |
| **SYS-04** ⭐ | abox+PipeWire audio loopback | `./scripts/run_loopback.sh` | audio flows through `hermes.abox` (`SIGNAL PRESENT`, peak ~50% FS) | PASS |
| SYS-05 | On-target ALSA→DSP→speaker | `./scripts/run_target.sh` (RK3588) | audible loopback, no growing xrun in `pw-top` | **BLOCKED** (no device) |

⭐ **SYS-04 evidence (recorded):** `pw-play → engine_in → hermes.abox(SRC→AEC→BEAM→SES) →
engine_out → pw-record`; null-sink virtual devices under `dbus-run-session`. Last run: 153 984
frames @ 48 kHz, peak 16383 (50% FS); L-channel passed (440 Hz present, 880 Hz attenuated ~1134× →
BEAM bypass confirmed). A 100→8000 Hz sweep variant: 202 240 frames @ 48 kHz, 50% FS.

---

## 10. Requirements Traceability Matrix (RTM)
Requirements per R1 (`ARCHITECTURE.md` Part I §6/§7). "Validated?" is the verification verdict for
*this baseline* — not whether code exists.

### 10.1 Functional requirements
| Req | Summary | Evidencing test(s) | Validated? |
|-----|---------|--------------------|:---:|
| FR-1 | Segment playback w/ casting | ABOX-09/11, SYS-04 (audio path only) | **PARTIAL** — transport path PASS; `PLAY_SEGMENT` render not built/tested |
| FR-2 | Wake-word → turn | INT-02 (kwd_wake_e2e) | **SKIPPED** — placeholder; VTS stub |
| FR-3 | Barge-in duck within budget | INT-01 (barge_in_e2e) | **SKIPPED** — placeholder; VAD emitter absent |
| FR-4 | STT · LLM route · TTS | — | **NOT COVERED** — connector stubs |
| FR-5 | Guardrails on every response | — | **NOT COVERED** — gate not built |
| FR-6 | Recall top-k facts | manual `HERMES_MEM_PING` smoke | **NOT COVERED** (automated) — manual only |
| FR-7 | Episodic log + consolidation | — | **NOT COVERED** — consolidation not built |
| FR-8 | Resume at position post-interaction | — | **NOT COVERED** — code path exists, untested |
| FR-9 | Offline operation | — | **NOT COVERED** |
| FR-10 | Parent view/erase memory | — | **NOT COVERED** — not built |
| FR-11 | Fault recovery (no full restart) | — | **NOT COVERED** — FSM `SS_FAULT` exists, untested |

### 10.2 Non-functional requirements
| Req | Target | Evidencing test(s) | Validated? |
|-----|--------|--------------------|:---:|
| NFR-1 | 5 ms / 240-frame locked quantum | SYS-01, SYS-04 (runs at locked quantum) | **PASS** (structural) |
| NFR-2 | IPC ≤ 2 ms (one mq hop) | test_msgbus (delivery/order; **no latency measurement**) | **NOT MEASURED** |
| NFR-3 | Barge-in ducking ≤ 12 ms | INT-01 | **NOT MEASURED** — path dormant |
| NFR-4 | Recall ≤ 150 ms p95 (local) | — | **NOT MEASURED** |
| NFR-5 | Consolidation off-turn | — | **N/A** — not built (by-design off-turn) |
| NFR-6 | Footprint < ~150 MB | — | **NOT MEASURED** |
| NFR-7 | Full offline playback + recall | — | **NOT COVERED** |
| NFR-8 | No alloc / no block on RT path | ABOX-07/10/11, SYS-04 (no xrun, no drops) | **PARTIAL** — behaviourally evidenced; not statically verified (no TSan/RT analyzer) |
| NFR-9 | Encryption at rest; consent for egress | — | **NOT COVERED** — not built |
| NFR-10 | Graceful degradation | test_msgbus (absent-peer); sidecar 503 path | **PARTIAL** — mq absent-peer PASS; memory-degrade manual |

**RTM rollup:** 1 FR PARTIAL, 2 FR SKIPPED, 8 FR NOT COVERED · 1 NFR PASS, 2 NFR PARTIAL, rest
NOT MEASURED / NOT COVERED / N/A. **No requirement is FAILED**; the gaps are *missing coverage on
unbuilt features*, consistent with R2 §18.5 ("architecture supports every use case; none end-to-end
runnable yet").

---

## 11. Coverage assessment
**Method.** Assertion-based functional verification; **no line/branch coverage instrumentation**
and **no sanitizer runs** are wired (A-3). Coverage below is *qualitative by component*.

**Exercised (real):** routing/mask logic; vDMA copy; mask-gated graph tick; buffer pool (sync,
incl. overrun soft-drop & rotation) and async per-slot workers; self-claim worker pool; lock-free
param store; reference-manager ring/alignment; SRC interpolation; full node graph (passthrough);
PipeWire transport (SYS-01/04); control bus delivery + lane order + absent-peer; lossless loopback.

**Not exercised (by design — kernels are passthrough stubs):** SRC drift correction under load;
AEC echo cancellation (PBFDAF + DTD); MVDR/GSC beamforming; SES spectral suppression. ABOX-11 /
SYS-04 *pin* the passthrough so a future signal-changing kernel is caught (RR-2).

**Not exercised (unbuilt / unwired):** cross-process E2E flows (INT-01/02), VTS KWD, VAD barge-in
emitter, cloud/local LLM+TTS, guardrail gate, consolidation, parent export, on-device acoustic
path (SYS-05).

---

## 12. Anomaly & deviation register
| ID | Severity | Finding | Disposition |
|----|----------|---------|-------------|
| **A-1** | Medium (reporting) | Prior report listed `barge_in_e2e`/`kwd_wake_e2e` as "✅ Pass"; they are `GTEST_SKIP` placeholders (assert nothing). | **Corrected** here → classified SKIPPED (§8). |
| **A-2** | Low (reporting) | R2 §18.3 cited "9/9 CTest pass" incl. "C++ DSP suites"; the C++ `dsp::Node` layer was deleted (D9/D10) — actual is **5 CTest cases**. | **Corrected** → 5 cases (§6). Recommend fixing the §18.3 figure in `ARCHITECTURE.md`. |
| **A-3** | Medium (process) | No sanitizer (ASan/UBSan/TSan) or coverage (gcov/lcov) instrumentation wired, despite a lock-free RT data plane. | **Open** → recommend adding ASan/UBSan + TSan CI jobs and a coverage target (§13 RR-3). |
| **A-4** | Info | SYS-05 not executable without RK3588 hardware. | **Accepted** → BLOCKED until device bench available. |
| **A-5** | Info (caveat) | Buffer pool *detects* overruns but only *absorbs* them once async pool-dispatch is wired (R2 §18.2). ABOX-07 covers the sync detect path. | **Tracked** in design; not a test defect. |

## 13. Limitations & residual risk
| ID | Residual risk | Impact | Mitigation / next test |
|----|---------------|--------|------------------------|
| RR-1 | KPI paths (barge-in, wake) unverified | core UX unproven | Implement INT-01/02 over the mq harness; add latency assertions for NFR-3. |
| RR-2 | ABOX-11/SYS-04 assume passthrough | bit-exact assertion breaks when real kernels land | Update expected vectors *with* each DSP kernel; add algorithmic metrics (ERLE/MOS/WER). |
| RR-3 | No memory-safety / data-race instrumentation | latent UB/races in lock-free RT code can ship | Add ASan+UBSan (functional) and TSan (concurrency) CI runs. |
| RR-4 | No on-device acoustic validation | host loopback ≠ ALSA/codec reality (drift, echo, xrun) | Execute SYS-05 on RK3588; add an xrun-rate soak. |
| RR-5 | Reference delay-lock seed-only | AEC won't converge on HW even after PBFDAF lands (R2 §18.4) | Add cross-correlation lock + §5 drift PI, then a convergence test. |

## 14. Conclusion & release recommendation
**Verification verdict:** the **data-plane engine** (graph, routing, pools, ref manager, param
store, SRC, soft-mute) and the **IPC contract** (CMsg/MsgBus/EventMap, lane order, absent-peer) are
**verified PASS** at unit level, and the **audio-transport spine** is validated live through
PipeWire (SYS-01..04). No test is FAILED.

**Validation verdict:** the SUT is **NOT yet end-to-end validated**. The two KPI integration paths
are SKIPPED placeholders (A-1), DSP algorithm correctness is unexercised (passthrough stubs), and
8/11 FRs have no coverage because the features are unbuilt.

**Recommendation: SUITABLE for continued development; NOT a release candidate.** Gate the next
milestone on closing RR-1 (KPI E2E) and RR-3 (sanitizer/coverage CI). This result is consistent
with the as-built audit (R2 §18.5): *architecture sound, implementation incomplete.*

## 15. Approval / sign-off
| Role | Name | Verdict | Date | Signature |
|------|------|---------|------|-----------|
| Verification owner | jhsieh | Recorded as above | 2026-06-29 | _pending_ |
| Engineering lead | _pending_ | _pending_ | | |
| QA / independent witness | _pending_ | _pending_ | | |

---

## Appendix A — Reproduction
```bash
./scripts/build.sh test          # unit + IPC suite (CTest) — §6/§7/§8
./scripts/build.sh run           # SYS-01 live PipeWire smoke
./scripts/build.sh cross         # SYS-02 RK3588 aarch64 build
./scripts/run_gui.sh             # SYS-03 web control console (then curl POST /api/cmd)
./scripts/run_loopback.sh        # SYS-04 abox+PipeWire loopback → out/loopback_out.wav
./scripts/run_target.sh ./hermes_abox 10   # SYS-05 (on RK3588 only)
```
Full procedures and pass criteria: `VALIDATION.md` (R4).

## Appendix B — Last executed run of record (2026-06-28, host: Apple Silicon, Docker 29.x)
```
CTest: 100% tests passed, 0 failed out of 5 (0.30s)
  abox_selftest ... Passed   (11 sub-cases, ABOX-01..11)
  test_eventmap ... Passed
  test_msgbus ..... Passed
  barge_in_e2e .... Passed   ← GTEST_SKIP (placeholder; see A-1 / §8)
  kwd_wake_e2e ..... Passed  ← GTEST_SKIP (placeholder; see A-1 / §8)
SYS-01 hermes_abox exit 124 → 0 (stayed connected)
SYS-02 aarch64 ELFs confirmed (file → ARM aarch64)
SYS-04 SIGNAL PRESENT, peak 50% FS, out/loopback_out.wav written
```
> Note: re-execution against baseline `19450a8` is recommended to refresh this evidence before
> sign-off; the SKIPPED classification is source-level and independent of any run.
