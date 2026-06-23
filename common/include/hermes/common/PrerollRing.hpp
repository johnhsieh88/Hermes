#pragma once
#include <atomic>
#include <cstdint>

// Shared-memory RAW-mic pre-roll ring (SDS §16.3). VTS is the sole producer
// (its own direct mic tap); ABOX is the sole consumer, on the WAKE_CONFIRMED
// interrupt. Lock-free SPSC, sample_pos-indexed. Mapped at /hermes.preroll by
// both processes. ABOX runs DSP on [pre-roll ⧺ live]; AEC is a no-op on the
// (idle, echo-free) pre-roll.
namespace hermes {

static constexpr int PREROLL_RING_SAMPLES = 16000 * 3;  // 3 s @ 16 kHz mono ≥ pre-roll + headroom
static constexpr int PREROLL_GUARD        = 1600;       // 100 ms overwrite guard band

struct PrerollRing {
    std::atomic<uint64_t> writePos;          // running sample_pos of next write
    std::atomic<uint32_t> epoch;             // bumped on xrun/reset → invalidates stale windows
    int16_t               pcm[PREROLL_RING_SAMPLES];  // RAW mic; VTS writes, ABOX reads → DSP
};

// VTS producer — one writer.
inline void Preroll_Write(PrerollRing* r, const int16_t* blk, int n) {
    uint64_t w = r->writePos.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) r->pcm[(w + i) % PREROLL_RING_SAMPLES] = blk[i];
    r->writePos.store(w + n, std::memory_order_release);
}

// ABOX consumer — on WAKE_CONFIRMED. Returns 0 on epoch mismatch (skip pre-roll).
inline int Preroll_Window(PrerollRing* r, uint64_t from, uint32_t epoch,
                          uint64_t* out_from, uint64_t* out_to) {
    uint64_t w = r->writePos.load(std::memory_order_acquire);
    if (r->epoch.load(std::memory_order_acquire) != epoch) return 0;
    if (w - from > (uint64_t)(PREROLL_RING_SAMPLES - PREROLL_GUARD))
        from = w - (PREROLL_RING_SAMPLES - PREROLL_GUARD);   // aged out → clamp oldest-safe
    *out_from = from;
    *out_to   = w;
    return 1;
}

// WAKE_CONFIRMED body (SDS §16.4).
struct WakeConfirmedBody {
    uint64_t wake_pos;          // sample_pos of detected keyword end (PipeWire timeline)
    uint64_t capture_from_pos;  // pre-roll start = wake_pos − pre_roll_margin
    uint32_t epoch;             // PrerollRing.epoch at detection
};

} // namespace hermes
