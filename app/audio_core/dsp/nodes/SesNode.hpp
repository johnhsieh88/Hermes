#pragma once
#include "audio_core/dsp/Node.hpp"

namespace hermes::dsp {

// Sound Enhancement Solution — noise suppression / dereverb / AGC on the mono
// stream. Runs after beamform so KWD (VTS) and STT see the cleanest signal.
class SesNode : public Node {
public:
    const char* name() const override { return "SES"; }
    void process(const AudioBuffer& in, AudioBuffer& out) override {
        // TODO: spectral noise suppression / dereverb / AGC.
        passthrough(in, out);   // BYPASS
    }
};

} // namespace hermes::dsp
