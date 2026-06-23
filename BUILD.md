# Building Hermes

**Target: Rockchip RK3588** — aarch64, 4× Cortex-A76 (big) + 4× Cortex-A55 (LITTLE).
arm64 is the only supported target. The PipeWire targets (`Pw.cpp`, `PwNode.cpp`,
`hermes_pw_*`) need `libpipewire-0.3` and a **Linux** host (PipeWire is Linux-only —
no native macOS build), so on a Mac you build inside Docker.

---

## 1. Docker on macOS → RK3588 (primary)
```bash
brew install colima docker && colima start          # one-time (a Linux VM for Docker)
docker build -t hermes-rk3588 -f docker/Dockerfile .
docker run --rm -v "$PWD":/src -w /src hermes-rk3588   # cross-builds → build-rk3588/
```
The image is Ubuntu + arm64 multiarch + `g++-aarch64-linux-gnu` + `libpipewire-0.3-dev:arm64`;
the default command cross-builds with `cmake/rk3588.toolchain.cmake`. Output binaries are
aarch64 (RK3588).

*Apple Silicon shortcut:* run a native arm64 container instead and build without the
cross prefix — faster, no multiarch:
```bash
docker run --rm --platform=linux/arm64 -v "$PWD":/src -w /src ubuntu:24.04 bash -c \
  "apt-get update && apt-get install -y cmake g++ pkg-config libpipewire-0.3-dev && \
   cmake -S . -B build && cmake --build build -j"
```

## 2. Yocto SDK route (meta-rockchip BSP — for the product image)
Inside your Yocto/Linux build container:
```bash
source /opt/poky/<ver>/environment-setup-aarch64-poky-linux   # sets CC/CXX/sysroot/PKG_CONFIG
cmake -S . -B build-rk3588 && cmake --build build-rk3588 -j    # PipeWire from the image sysroot
```
No toolchain file needed — the SDK provides the cross toolchain and a sysroot that already
contains `pipewire`. The framework also ships as a Yocto recipe (`hermes.bb`) depending on
`pipewire`, `alsa-lib`, and the PREEMPT_RT kernel (SDS §9.5).

## 3. Cross-compile directly (Linux host, no Docker)
```bash
sudo apt-get install -y g++-aarch64-linux-gnu pkg-config cmake
cmake -S . -B build-rk3588 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/rk3588.toolchain.cmake \
      -DCMAKE_SYSROOT=/path/to/rk3588-sysroot      # sysroot with libpipewire-0.3-dev:arm64
cmake --build build-rk3588 -j
```
(Omit `CMAKE_SYSROOT` to use host multiarch arm64 packages instead.)

## 4. CI
`.github/workflows/ci.yml` cross-compiles the full tree for **arm64** on every push
(`g++-aarch64-linux-gnu` + `libpipewire-0.3-dev:arm64`, `rk3588.toolchain.cmake`) — this
is where the PipeWire code is compile-verified.

---

## ⚠ RK3588 CPU topology — remap the SDS affinity

The SDS §2 / §9.5 assumed **big cores = CPU0–3**. On **RK3588 it is inverted**:

| Cores | Cluster | Role |
|-------|---------|------|
| **CPU4–7** | Cortex-**A76** (big) | hard-RT DSP island, PipeWire data-loop + worker pool |
| **CPU0–3** | Cortex-**A55** (LITTLE) | 1 ms control scheduler + background workers / MsgBus |

So the §9.5 kernel cmdline and `Thread_Pin` affinities must target the **A76 cores 4–7**
for the DSP, e.g. `isolcpus=4-7 nohz_full=4-7 rcu_nocbs=4-7 irqaffinity=0-3`, with the
ALSA/I2S IRQ affined to an A76 (e.g. CPU4). Treat the SDS core numbers as a template and
substitute the RK3588 mapping (big = 4–7, LITTLE = 0–3).
