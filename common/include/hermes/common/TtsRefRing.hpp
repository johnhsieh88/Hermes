#pragma once
#include <atomic>
#include <cstdint>

// Shared-memory SPSC ring: LLM_CONNECTOR TTS playback → ABOX AEC reference.
// Mapped at /hermes.ttsref. LLM_CONNECTOR is the sole producer (s_playback RT
// callback writes exactly what is sent to the PipeWire sink each quantum);
// ABOX is the sole consumer (AboxCtx::process, before abox_pipeline_process_tick),
// which resamples from TTSREF_RATE → 48 kHz and calls abox_ref_write_farend().
//
// Both processes open with O_CREAT | O_RDWR so startup order doesn't matter;
// the kernel zero-initialises fresh shm pages which matches the wp=rp=0 idle state.
namespace hermes {

static constexpr uint32_t TTSREF_RATE     = 22050;             // Piper native rate
static constexpr uint32_t TTSREF_CAPACITY = TTSREF_RATE * 2;  // 2 s at 22050 Hz

struct TtsRefRing {
    std::atomic<uint32_t> wp{0};
    std::atomic<uint32_t> rp{0};
    float buf[TTSREF_CAPACITY]{};
};

// LLM_CONNECTOR producer — RT-safe, no locks.
inline void TtsRef_Write(TtsRefRing* r, const float* src, uint32_t n) {
    uint32_t w = r->wp.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n; ++i) {
        r->buf[w] = src[i];
        if (++w == TTSREF_CAPACITY) w = 0;
    }
    r->wp.store(w, std::memory_order_release);
}

// ABOX consumer — RT-safe, no locks. Returns samples actually read (< n if ring starved).
inline int TtsRef_Read(TtsRefRing* r, float* dst, int n) {
    uint32_t w  = r->wp.load(std::memory_order_acquire);
    uint32_t rd = r->rp.load(std::memory_order_relaxed);
    uint32_t av = (w - rd + TTSREF_CAPACITY) % TTSREF_CAPACITY;
    if (static_cast<uint32_t>(n) > av) n = static_cast<int>(av);
    for (int i = 0; i < n; ++i) {
        dst[i] = r->buf[rd];
        if (++rd == TTSREF_CAPACITY) rd = 0;
    }
    r->rp.store(rd, std::memory_order_release);
    return n;
}

} // namespace hermes
