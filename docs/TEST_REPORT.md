# Hermes — Test Report

Test cases and their status for the Hermes audio DSP framework. Covers the unit suite
(GoogleTest C++ + the C `abox_selftest`) and the system/integration checks. See
[../VALIDATION.md](../VALIDATION.md) for the end-to-end validation process and exact commands.

- **Last run:** 2026-06-28 · host: Apple Silicon (arm64) in `ubuntu:24.04` container · Docker 29.x
- **Unit result:** ✅ **5/5 ctest pass** (0.30s)
- **How to run:** `./scripts/build.sh test` (unit) · `./scripts/run_loopback.sh` (audio loopback)

---

## 1. Unit tests (CTest)

`cmake --build build && ctest --test-dir build --output-on-failure`

| # | Test (CTest) | Lang | Covers | Status |
|---|--------------|------|--------|--------|
| 1 | `abox_selftest` | C | data-plane suite (11 sub-cases, see §1.1) | ✅ Pass |
| 2 | `test_eventmap` | C++ | EventMap dispatch (id → handler) | ✅ Pass |
| 3 | `test_msgbus` | C++ | MsgBus POSIX-mq send/recv contract | ✅ Pass |
| 4 | `barge_in_e2e` | C++ | barge-in event sequence | ✅ Pass |
| 5 | `kwd_wake_e2e` | C++ | keyword-wake session flow | ✅ Pass |

### 1.1 `abox_selftest` data-plane sub-cases

| TC ID | Sub-case | Asserts | Status |
|-------|----------|---------|--------|
| ABOX-01 | `test_routing_mask` | per-mode `active_pipeline_mask` (idle=0, cloud=all, barge keeps AEC, no TTS) | ✅ |
| ABOX-02 | `test_vdma` | vDMA xfer primitive + node form (ingress IN / egress OUT) copy correctness | ✅ |
| ABOX-03 | `test_graph_mask_gating` | graph tick runs/skips stages by mode mask | ✅ |
| ABOX-04 | `test_reference_manager` | AEC ref delay ring: zero-delay identity, shift, VI-Sense | ✅ |
| ABOX-05 | `test_worker_pool` | self-claim worker pool runs every slice exactly once | ✅ |
| ABOX-06 | `test_param_store` | lock-free param double-buffer publish/swap | ✅ |
| ABOX-07 | `test_buffer_pipeline` | Core-Proportional pool: ingest→tick→egress, overrun soft-drop, core rotation | ✅ |
| ABOX-08 | `test_src_node` | SRC: unity identity + fractional-resample interpolation, bounded/no-NaN | ✅ |
| ABOX-09 | `test_playback_pipeline` | full src→aec→beam→ses sync path, bounded output, no drops | ✅ |
| ABOX-10 | `test_async_pipeline` | per-slot worker threads produce real output across periods | ✅ |
| **ABOX-11** | **`test_loopback_bypass`** ⭐ **(basic TC)** | **full-graph loopback is bit-exact `output == L input`; beamform bypass (not L+R average); zero drops** | ✅ |

⭐ **ABOX-11** is the unit-level twin of the on-target audio loopback (TC SYS-04). It feeds a
tone on the L mic and a different DC level on the R mic through every active node
(CLOUD_STREAMING mode) and asserts the mono output equals the L input **bit-exact** — proving
the transport+graph path is lossless and that every stub node passes audio through unchanged
(`output buffer == input buffer`).

---

## 2. System / integration tests

Environment-dependent (Docker + PipeWire, or the RK3588 device). Run via the scripts; see
VALIDATION.md for pass criteria.

| TC ID | Test | Command | Proves | Status |
|-------|------|---------|--------|--------|
| SYS-01 | Live PipeWire smoke | `./scripts/build.sh run` | engine connects + runs RT loop, no crash | ✅ Pass (exit 124→0) |
| SYS-02 | RK3588 cross-build | `./scripts/build.sh cross` | builds aarch64 device binaries | ✅ Pass (ARM aarch64 ELF) |
| SYS-03 | Web control console | `./scripts/run_gui.sh` + browser | UI action → control CMsg on the bus | ✅ Pass (`POST /api/cmd → [sent]`) |
| **SYS-04** | **abox + PipeWire audio loopback** ⭐ **(basic TC)** | `./scripts/run_loopback.sh` | **audio flows through `hermes.abox` via a real PipeWire graph** | ✅ Pass (`SIGNAL PRESENT`, peak 50% FS) |
| SYS-05 | On-target playback | `./scripts/run_target.sh` (on RK3588) | ALSA→DSP→speaker, audible, no xruns | ⏳ Device-only (pending hardware) |

⭐ **SYS-04** (basic TC) — `pw-play → engine_in → hermes.abox(src→aec→beam→ses) → engine_out →
pw-record`, captured to `out/loopback_out.wav`. Runs on a Mac with no audio hardware (timer-
driven null-sink virtual devices under `dbus-run-session`). `afplay out/loopback_out.wav` to
hear the engine-processed audio. Last run: 153984 frames @ 48 kHz, peak 16383 (50% FS), L
channel passed through (440 Hz present, 880 Hz 1134× down → beamform bypass confirmed).

---

## 3. Coverage notes — what is and isn't exercised

**Exercised (real):** routing/mask, vDMA, buffer pool (sync + async), worker pool, param
store, reference manager, SRC interpolation, the full node graph, the PipeWire transport, the
control bus, and the lossless loopback (output == input) through all nodes.

**Not yet exercised (kernels TODO — nodes bypass by design):** SRC drift correction under
load, AEC echo cancellation (PBFDAF + DTD), MVDR beamforming, SES spectral suppression. Each
is a clean passthrough until implemented; ABOX-11 / SYS-04 pin that passthrough so a future
kernel that changes the signal is caught by the now-failing bit-exact assertion (update the TC
alongside the kernel).
