#pragma once
#include <cstdint>
#include <cstring>

// DSP node framework (SDS §4.0). Every stage implements this common interface;
// the Chain (§9.1/§10) drives them uniformly. For the scaffold each node BYPASSES
// (input copied straight to output); real DSP fills the process() bodies later.
namespace hermes::dsp {

static constexpr int MAX_CHANNELS = 2;     // 2-mic array
static constexpr int MAX_BLOCK    = 512;   // FFT-framing headroom over the 240-sample block

// Engine use-case mode (SDS §3). Approach B: nodes adapt behavior by mode on a
// static graph (no re-routing). Carried per-block in AudioBuffer, latched
// coherently from ModeControl (ModeControl.hpp).
enum class EngineMode : int {
    KeywordListening = 0,   // idle/wake — no playback, AEC bypassed
    BargeInMuting    = 1,   // user interrupted — ducking; AEC active
    CloudStreaming   = 2,   // conversation — full duplex, AEC active
    SystemResetError = 3,
};

// Channel-major audio buffer flowing through the chain. Engine-owned storage
// (the RT path treats these as pool buffers — zero-alloc).
struct AudioBuffer {
    float      chan[MAX_CHANNELS][MAX_BLOCK];
    int        channelCount = 0;
    int        sampleCount  = 0;
    uint64_t   samplePos    = 0;                       // PipeWire timeline (clock.position)
    EngineMode mode         = EngineMode::KeywordListening;  // latched per cycle (approach B)
};

class Node {
public:
    virtual ~Node() = default;
    virtual const char* name() const = 0;
    virtual void prepare(int sampleRate, int blockSize) { (void)sampleRate; (void)blockSize; }
    virtual void reset() {}
    // Consume ONE input buffer, produce ONE output buffer.
    virtual void process(const AudioBuffer& in, AudioBuffer& out) = 0;

protected:
    // BYPASS helper: identity passthrough (input → output).
    static void passthrough(const AudioBuffer& in, AudioBuffer& out) {
        out.channelCount = in.channelCount;
        out.sampleCount  = in.sampleCount;
        out.samplePos    = in.samplePos;
        out.mode         = in.mode;
        for (int c = 0; c < in.channelCount; ++c)
            std::memcpy(out.chan[c], in.chan[c], sizeof(float) * static_cast<size_t>(in.sampleCount));
    }
};

} // namespace hermes::dsp
