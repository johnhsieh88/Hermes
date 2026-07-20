#pragma once
#include <atomic>
#include <cstdint>

// AudioRing<N> — the standard consumer-edge audio buffer (ARCHITECTURE §13.2 "out_0 contract").
//
// Rate decoupling between the locked 5 ms graph cadence and a consumer's own pace lives HERE,
// one ring per consumer, in the consumer's process — never inside abox (the engine publishes
// at graph cadence and knows nobody's chunk size). This is the house pattern already used by
// VTS's mic tap and the PrerollRing; this header makes it a single shared, *correct* component.
//
// Contract:
//   - Lock-free SPSC. ONE producer (a PipeWire RT callback: push only, ~µs, no alloc, no lock),
//     ONE consumer (a worker thread: pop in its own chunk size — STT ~100 ms, VAD ~32 ms).
//   - Overflow policy: DROP-NEW + counter. A stalled consumer loses its own tail; the RT
//     callback never blocks, never laps the reader (unlike a bare overwriting ring, which
//     corrupts audio silently when the consumer falls behind).
//   - Sizing: give it 1–2 s of headroom (orders of magnitude above real scheduling jitter,
//     cheap in RAM). 48 kHz mono f32: N = 96000 ≈ 2 s ≈ 384 KB.
namespace hermes {

template <int N>
class AudioRing {
    static_assert(N > 0, "capacity must be positive");

public:
    // Producer side (RT callback). Appends n samples; if the ring cannot hold them all,
    // drops the excess (newest samples) and counts them. Never blocks, never allocates.
    // Returns the number of samples actually written.
    int push(const float* src, int n) {
        const uint64_t w = wp_.load(std::memory_order_relaxed);
        const uint64_t r = rp_.load(std::memory_order_acquire);
        int free_ = static_cast<int>(N - (w - r));
        int wr = n < free_ ? n : free_;
        for (int i = 0; i < wr; ++i) buf_[(w + i) % N] = src[i];
        wp_.store(w + wr, std::memory_order_release);
        if (wr < n) overruns_.fetch_add(n - wr, std::memory_order_relaxed);
        const uint64_t used = (w + wr) - r;          // occupancy after this push
        if (used > hw_.load(std::memory_order_relaxed))
            hw_.store(used, std::memory_order_relaxed);   // producer-only writer → no race
        return wr;
    }

    // Consumer side (worker thread). Pops up to max samples; returns the count (0 if empty).
    int pop(float* dst, int max) {
        const uint64_t w = wp_.load(std::memory_order_acquire);
        const uint64_t r = rp_.load(std::memory_order_relaxed);
        int avail = static_cast<int>(w - r);
        int rd = max < avail ? max : avail;
        for (int i = 0; i < rd; ++i) dst[i] = buf_[(r + i) % N];
        rp_.store(r + rd, std::memory_order_release);
        return rd;
    }

    // Discard everything buffered so far. MUST be called from the CONSUMER thread only —
    // it writes rp_, and rp_ has exactly one writer (a control-thread caller would race
    // pop() and can move rp_ backwards). If another thread wants a drain, it must signal
    // the consumer (flag/generation) and let the consumer call this.
    void drain() { rp_.store(wp_.load(std::memory_order_acquire), std::memory_order_release); }

    int available() const {
        return static_cast<int>(wp_.load(std::memory_order_acquire) -
                                rp_.load(std::memory_order_acquire));
    }

    // Total samples dropped at the producer because the consumer lagged. A nonzero value is a
    // diagnosable event (log it / surface it as a counter), never a silent corruption.
    uint64_t overruns() const { return overruns_.load(std::memory_order_relaxed); }

    // Peak occupancy ever observed (samples). Healthy: a few blocks. Climbing toward N
    // means the consumer is falling behind — the early warning before overruns() moves.
    int highWater() const { return static_cast<int>(hw_.load(std::memory_order_relaxed)); }

    static constexpr int capacity() { return N; }

private:
    float buf_[N]{};
    std::atomic<uint64_t> wp_{0};        // total samples ever written (producer-owned)
    std::atomic<uint64_t> rp_{0};        // total samples ever read (consumer-owned)
    std::atomic<uint64_t> overruns_{0};  // samples dropped on full
    std::atomic<uint64_t> hw_{0};        // peak occupancy (producer-maintained)
};

} // namespace hermes
