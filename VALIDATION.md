# Hermes — Validation Process

End-to-end validation for the Hermes audio DSP framework: from a clean checkout to a
verified RK3588 playback path. Every step has an explicit **pass criterion** so a run is
unambiguously green or not.

Hermes builds in two arch contexts:
- **Native arm64** — runs on this host (Apple Silicon, or any arm64 Linux). Used for unit
  tests and the live PipeWire smoke. No emulation on M1 (the host *is* aarch64).
- **RK3588 cross (aarch64)** — produces device binaries via an aarch64 cross-toolchain.
  This is a *compile*, nothing aarch64 executes, so **no QEMU is involved**.

See [BUILD.md](./BUILD.md) for the underlying build details and [docs/](./docs) for
architecture.

---

## 0. Prerequisites — Docker setup

| Need | macOS (Apple Silicon) | Linux |
|------|------------------------|-------|
| Docker Engine ≥ 24 | Docker Desktop or `colima start` | distro Docker / Podman |
| Disk | ~2 GB for the Ubuntu + cross-toolchain image | same |

Verify Docker is up:

```bash
docker version --format '{{.Server.Version}}'   # any 24+ is fine
```

**On Apple Silicon, no QEMU install is required:**
- Native `test`/`run`/`native` use `--platform=linux/arm64` → run **natively** on M-series.
- The RK3588 cross image is pinned to `linux/amd64` for cross-platform determinism; Docker
  Desktop emulates amd64 for that build automatically (Rosetta/QEMU, bundled — slower, but
  you install nothing). The *emitted* binaries are aarch64.

---

## 1. Native unit tests (ctest) — the data-plane contract

Builds the C data plane + C++ control plane in an arm64 container and runs the full test
suite (the C `abox_selftest` plus the GoogleTest C++ suites).

```bash
./scripts/build.sh test
```

**Pass criterion:** `100% tests passed, 0 tests failed out of 5`:

```
1/5 abox_selftest ....... Passed   # routing mask, vDMA, graph gating, ref mgr,
2/5 test_eventmap ....... Passed   #   worker pool, param store, buffer pipeline,
3/5 test_msgbus ......... Passed   #   SRC node, playback pipeline, async pipeline
4/5 barge_in_e2e ........ Passed
5/5 kwd_wake_e2e ........ Passed
```

The script returns non-zero if any test fails (`ctest --output-on-failure`).

---

## 2. Live PipeWire smoke — the engine actually connects

Starts a real PipeWire daemon in the container and runs `hermes_abox` against it for a few
seconds. Exercises the live data path: one `pw_filter` hosting the abox graph, the async
buffer pool + worker threads, vDMA ingest/egress.

```bash
./scripts/build.sh run        # RUN_SECS=5 by default; override: RUN_SECS=10 ./scripts/build.sh run
```

**Pass criterion:** script exits **0**. Internally `hermes_abox` is killed by `timeout`
after `RUN_SECS` — exit `124` means *it stayed connected to PipeWire the whole time*
(the good case), which the script maps to `0`. Exit `1` means it could not reach PipeWire.
A segfault/abort would surface as a different non-zero code.

---

## 3. RK3588 cross-compile — device binaries

Cross-compiles every process for aarch64 with the RK3588 toolchain
(`-mcpu=cortex-a76.cortex-a55`).

```bash
./scripts/build.sh cross
```

**Pass criterion:** `100%` targets built, and the outputs are aarch64 ELFs:

```bash
file build-rk3588/app/hermes_abox
# ELF 64-bit LSB pie executable, ARM aarch64 ... for GNU/Linux
```

### 3b. Running it straight from Docker Desktop (no CLI)

The image bakes the repo in (`COPY . /src`), so the **Run** button works with no bind
mount. To keep the produced binaries, **do not** enable auto-remove (`--rm`): with `--rm`
the container's writable layer (holding `build-rk3588/`) is discarded on exit. Either:
- Run without `--rm`, then **Docker Desktop → Containers → Files → `/src/build-rk3588`**
  (or `docker cp <container>:/src/build-rk3588 ./out`), **or**
- Use the CLI bind-mount flow so artifacts land on the host directly:
  ```bash
  docker run --rm -v "$PWD":/src -w /src hermes-rk3588
  ```

If `/src` is ever empty (e.g. an empty dir mounted over it) the container fails fast with
an actionable message instead of cmake's cryptic "source directory does not contain
CMakeLists.txt".

---

## 4. On-target playback validation (RK3588 device) — ALSA → speaker

Run **on the RK3588**, not the host. Requires PipeWire + WirePlumber running on the device.
Validates the full audio path end-to-end (DSP nodes are passthrough stubs for now, so you
should hear the mic out the speaker, delayed ~1 block).

```bash
# copy build-rk3588/app/hermes_abox to the device, then on the device:
./scripts/run_target.sh ./hermes_abox 10
```

It starts the engine, prints `hermes.abox` ports, links `capture_FL/FR → hermes.abox:in_0/1`
and `hermes.abox:out_0 → playback_FL/FR`, and holds the path live for N seconds.

**Pass criterion:**
- `pw-link` shows the `hermes.abox` ports (`in_0`, `in_1`, `out_0`).
- Links establish without error.
- Audible: speaking into the mic comes out the speaker (~1 block latency).
- `pw-top` shows the `hermes.abox` row with **no growing xrun count**.

No mic on the bench? Feed a file: `pw-play --target hermes.abox file.wav`.

---

## 5. Interactive use-case validation — the web test console

A browser GUI (`hermes_gui_interface`, ModuleId 7) drives the basic use cases by hand. Every
button translates to a control `CMsg` on the **same POSIX-mq bus** the real modules use, so it
genuinely exercises the control plane. Audio is fed into the engine via `pw-play`.

```bash
# in a Linux env with the native build + PipeWire (e.g. the arm64 container):
./scripts/run_gui.sh           # starts pipewire + hermes_abox + the GUI bridge
# → open http://localhost:8080
```

Use cases exposed:
- **Select a sample + Play / Stop** — feeds `samples/*.wav` into `hermes.abox` (`pw-play`), sends `START_CAPTURE`/`STOP_CAPTURE`.
- **Engine mode** (Keyword Listening / Barge-In Muting / Cloud Streaming / System Reset) → `SET_MODE` to AUDIO_CORE.
- **Volume** slider → `SET_VOLUME` → real master gain (0–1.5×) in the RT egress.
- **Barge-In** → `DUCK_PLAYBACK` + `SET_MODE` on the URGENT lane.
- **Start / Cancel Session** → `START_SESSION` / `CANCEL_SESSION` to SUPERVISOR.
- **Live event feed** — inbound bus events (MODE_CHANGED, SOFT_MUTE, XRUN, …) shown via `GET /api/events`.

**Pass criterion:** with the engine running, `POST /api/cmd` returns `"ok":true … [sent]`
(the CMsg reached the peer's mq). With no engine, it returns `[no peer]` and the bridge
stays up. Verified headless via `curl`:

```
{"ok":true,"msg":"SET_MODE=2 → AUDIO_CORE  [sent]"}
{"ok":true,"msg":"SET_VOLUME=0.80 → AUDIO_CORE  [sent]"}
{"ok":true,"msg":"BARGE-IN (DUCK_PLAYBACK + SET_MODE=1) → AUDIO_CORE  [sent]"}
```

> The GUI bridge is a **host/dev test tool** — it uses TCP/HTTP + `fork`/`pw-play` and is not
> part of the RK3588 device image.

## 6. Audio loopback through the engine — on the Mac, no hardware

Validates that audio genuinely flows **through `hermes_abox` via PipeWire** — the closest
thing to the on-device ALSA→DSP→speaker path that runs on a Mac. A container has no audio
device, so instead of a speaker the engine output is captured to a WAV you can hear with
`afplay`.

```bash
./scripts/run_loopback.sh                       # → out/loopback_out.wav
afplay out/loopback_out.wav                     # hear the engine-processed audio (macOS)
SAMPLE=/src/samples/beeps_x3.wav ./scripts/run_loopback.sh   # try another clip
```

How it works (all inside the arm64 container):
```
pw-play → engine_in (null sink) ─monitor→ hermes.abox (src→aec→beamform→ses) → engine_out (null sink) → pw-record
```
Two non-obvious things this handles (both discovered the hard way):
- **WirePlumber needs a D-Bus session bus** or it crashes (`Cannot autolaunch D-Bus without
  X11 $DISPLAY`) — the script wraps the daemons in `dbus-run-session`.
- **`hermes.abox` is a non-autoconnect Filter** (Model B; links are explicit). The script
  creates two timer-driven **null-sink virtual devices** with stable ports and `pw-link`s
  the graph through the engine. Without a driving device, the graph stays suspended and
  never even creates ports.

**Pass criterion:** the script prints `SIGNAL PRESENT — abox+PipeWire loopback OK` and a
non-trivial peak (e.g. `peak=16383 (50.0% FS)`), and `out/loopback_out.wav` is a real
multi-KB clip. Verified: a 100→8000 Hz sweep returned 202240 frames @ 48 kHz, 50% FS.

> Not audible *in real time* on Mac (no audio device, no QEMU helps — it's a missing device,
> not a missing CPU). The 2-step capture→`afplay` is the Mac-side stand-in for the speaker;
> real-time audible loopback is the RK3588 (step 4).

## Validation matrix — what each step proves

| Step | Command | Proves | Pass |
|------|---------|--------|------|
| 1 | `build.sh test`  | data-plane + control logic correct | 5/5 ctest |
| 2 | `build.sh run`   | live engine connects & runs RT loop | exit 0 (124→0) |
| 3 | `build.sh cross` | builds for the real target | aarch64 ELFs |
| 3b| Docker Desktop Run | one-click build, no CLI | `>> Done.` printed |
| 4 | `run_target.sh`  | ALSA→DSP→speaker path on device | audible + no xruns |
| 5 | `run_gui.sh` + browser | interactive use cases over the bus | `POST /api/cmd` → `[sent]` |
| 6 | `run_loopback.sh` + `afplay` | audio flows through abox via PipeWire | `SIGNAL PRESENT`, audible clip |

## Last validated run (host: Apple Silicon, Docker 29.x)

- **Step 1:** `100% tests passed, 0 failed out of 5` (0.30s).
- **Step 2:** `hermes_abox` exit 124 → mapped to 0 (stayed connected, no segfault).
- **Step 3:** all targets built; `hermes_abox`/`hermes_supervisor` confirmed `ARM aarch64`.
- **Step 3b:** no-mount `docker run` reached `>> Done.` (Docker Desktop Run button path).
- **Step 4:** requires physical RK3588 — run on device.
