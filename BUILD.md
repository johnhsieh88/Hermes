# Building Hermes

The control plane + DSP framework build on any host. The **PipeWire targets**
(`Pw.cpp`, `PwNode.cpp`, `hermes_pw_*`) need `libpipewire-0.3` and therefore a
**Linux** host (PipeWire is Linux-only — no native macOS build).

Target board: **Rockchip RK3588** — aarch64, 4× Cortex-A76 (big) + 4× Cortex-A55 (LITTLE).

---

## 1. Native Linux build (x86_64) — verifies the PipeWire targets
```bash
sudo apt-get install -y cmake g++ pkg-config libpipewire-0.3-dev libgtest-dev
cmake -S . -B build && cmake --build build -j
ctest --test-dir build --output-on-failure
```

## 2. From macOS via a Linux container (no native PipeWire on macOS)
```bash
brew install colima docker && colima start          # one-time
docker build -t hermes-build -f docker/Dockerfile .
docker run --rm -v "$PWD":/src -w /src hermes-build \
    bash -c "cmake -S . -B build && cmake --build build -j"
```

## 3. Cross-compile for RK3588 (arm64)

**Route A — Debian/Ubuntu arm64 sysroot + cross GCC (quickest):**
```bash
sudo apt-get install -y g++-aarch64-linux-gnu pkg-config cmake
# Provide an arm64 sysroot that has libpipewire-0.3-dev:arm64 (Armbian/Ubuntu-Rockchip
# rootfs, or `debootstrap --arch=arm64`, or multiarch with CMAKE_SYSROOT=/).
cmake -S . -B build-rk3588 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/rk3588.toolchain.cmake \
      -DCMAKE_SYSROOT=/path/to/rk3588-sysroot
cmake --build build-rk3588 -j
```

**Route B — Yocto SDK (recommended for the product, `meta-rockchip` BSP):**
```bash
source /opt/poky/<ver>/environment-setup-aarch64-poky-linux   # sets CC/CXX/sysroot/PKG_CONFIG
cmake -S . -B build-rk3588 && cmake --build build-rk3588 -j    # PipeWire from the image sysroot
```
The framework also ships as a Yocto recipe (`hermes.bb`) depending on `pipewire`,
`alsa-lib`, and the RT kernel (SDS §9.5).

## 4. CI

`.github/workflows/ci.yml` runs on every push: a native **x86_64** job (builds the
PipeWire targets + tests) and an **arm64 cross** job (`g++-aarch64-linux-gnu` +
`libpipewire-0.3-dev:arm64`). This is where the PipeWire code is actually verified.

---

## ⚠ RK3588 CPU topology — remap the SDS affinity

The SDS §2 / §9.5 assumed **big cores = CPU0–3**. On **RK3588 it is inverted**:

| Cores | Cluster | Role |
|-------|---------|------|
| **CPU4–7** | Cortex-**A76** (big) | hard-RT DSP island, PipeWire data-loop + worker pool |
| **CPU0–3** | Cortex-**A55** (LITTLE) | 1 ms control scheduler + background workers / MsgBus |

So the §9.5 kernel cmdline and `Thread_Pin` affinities must target the **A76 cores 4–7** for the DSP, e.g.:
```
isolcpus=4-7 nohz_full=4-7 rcu_nocbs=4-7 irqaffinity=0-3
```
and the ALSA/I2S IRQ affined to one A76 (e.g. CPU4). Treat the SDS core numbers as a
template and substitute the RK3588 mapping (big = 4–7, LITTLE = 0–3).
