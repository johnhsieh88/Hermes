#pragma once
#include <atomic>
#include <cstdint>
#include "audio_core/dsp/Node.hpp"

// Sample-aligned engine-mode control — approach B (SDS §4 mode-graphs realized as
// per-node modes on a STATIC graph, no re-routing). The control side (SUPERVISOR /
// AudioCore handler) sets a new mode effective at a future sample position; every
// node computes the SAME effective mode for a given block via modeAt(samplePos),
// so a mid-stream use-case change is:
//   • cross-node coherent — all nodes switch at the same sample boundary regardless
//     of execution order or which cycle they run in (no skew),
//   • block-aligned and bounded (≤ 1 quantum), never mid-buffer,
//   • glitch-free at the graph level (nothing is re-linked).
// Lock-free: atomics only, safe for the RT data-loop to read.
namespace hermes::dsp {

class ModeControl {
public:
    // RT side: the effective mode for a block starting at `samplePos`.
    EngineMode modeAt(uint64_t samplePos) const {
        return (samplePos >= switchPos_.load(std::memory_order_acquire))
                   ? static_cast<EngineMode>(next_.load(std::memory_order_acquire))
                   : static_cast<EngineMode>(prev_.load(std::memory_order_acquire));
    }

    // Control side (non-RT): apply `m` starting at sample `atSamplePos`
    // (0 or a past position = take effect on the next block).
    void setMode(EngineMode m, uint64_t atSamplePos) {
        prev_.store(next_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        next_.store(static_cast<int>(m), std::memory_order_relaxed);
        switchPos_.store(atSamplePos, std::memory_order_release);
    }

    EngineMode target() const {
        return static_cast<EngineMode>(next_.load(std::memory_order_relaxed));
    }

private:
    std::atomic<int>      prev_{static_cast<int>(EngineMode::KeywordListening)};
    std::atomic<int>      next_{static_cast<int>(EngineMode::KeywordListening)};
    std::atomic<uint64_t> switchPos_{0};
};

} // namespace hermes::dsp
