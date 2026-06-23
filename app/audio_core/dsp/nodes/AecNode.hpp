#pragma once
#include "audio_core/dsp/Node.hpp"

namespace hermes::dsp {

// Acoustic Echo Cancellation (SDS §4.3) — Partitioned-Block Frequency-Domain
// adaptive filter. Cancels the loudspeaker echo from the mic using the time-aligned,
// post-fader far-end reference; frozen by the DTD during double-talk.
//
// Approach B (mode-adaptive, static graph): cancel during CloudStreaming / barge-in;
// bypass while only KeywordListening (no playback → no echo). The active↔bypass mix
// is ramped over ~10 ms so a mid-stream mode change does not click — and nothing is
// re-routed.
class AecNode : public Node {
public:
    const char* name() const override { return "AEC"; }

    void setReference(const AudioBuffer* ref) { ref_ = ref; }   // far-end (§4.3.1)

    void process(const AudioBuffer& in, AudioBuffer& out) override {
        const bool  active = (in.mode == EngineMode::CloudStreaming ||
                              in.mode == EngineMode::BargeInMuting);
        const float target = active ? 1.0f : 0.0f;
        const float rate   = 1.0f / 480.0f;   // ~10 ms ramp @ 48 kHz

        out.channelCount = in.channelCount;
        out.sampleCount  = in.sampleCount;
        out.samplePos    = in.samplePos;
        out.mode         = in.mode;

        float endMix = mix_;
        for (int c = 0; c < in.channelCount; ++c) {
            float m = mix_;                                  // each channel ramps identically
            for (int i = 0; i < in.sampleCount; ++i) {
                m += (target > m) ? rate : (target < m ? -rate : 0.0f);
                if (m < 0.0f) m = 0.0f; else if (m > 1.0f) m = 1.0f;
                const float cancelled = in.chan[c][i];       // TODO: real PBFDAF echo-cancelled sample
                const float bypass    = in.chan[c][i];
                out.chan[c][i] = m * cancelled + (1.0f - m) * bypass;
            }
            endMix = m;
        }
        mix_ = (in.channelCount > 0) ? endMix
                                     : (target > mix_ ? mix_ + rate : mix_ - rate);
    }

private:
    const AudioBuffer* ref_ = nullptr;
    float mix_ = 0.0f;   // 0 = bypass, 1 = active (cancelling) — ramped across blocks
    // TODO: W[AEC_PARTITIONS][AEC_BINS*2], Xfifo, binPower, mu, leak, DTD freeze (§4.3)
};

} // namespace hermes::dsp
