#pragma once
#include "audio_core/dsp/Node.hpp"

namespace hermes::dsp {

// Beamforming (SDS §4.2) — combines the echo-cancelled channels into one enhanced
// mono stream, steered toward the target azimuth.
class BeamformNode : public Node {
public:
    const char* name() const override { return "Beamform"; }
    void process(const AudioBuffer& in, AudioBuffer& out) override {
        // TODO: delay-sum / filter-and-sum steered to targetAzimuth.
        // BYPASS: take channel 0 as the mono output (N → 1).
        out.channelCount = 1;
        out.sampleCount  = in.sampleCount;
        out.samplePos    = in.samplePos;
        std::memcpy(out.chan[0], in.chan[0], sizeof(float) * static_cast<size_t>(in.sampleCount));
    }
};

} // namespace hermes::dsp
