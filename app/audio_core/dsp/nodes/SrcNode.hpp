#pragma once
#include "audio_core/dsp/Node.hpp"

namespace hermes::dsp {

// Sample Rate Converter (SDS §4.1). Compensates ADC↔DAC crystal drift so the
// reference is sample-aligned to the mic before AEC.
class SrcNode : public Node {
public:
    const char* name() const override { return "SRC"; }
    void process(const AudioBuffer& in, AudioBuffer& out) override {
        // TODO: drift-tracking polyphase resample (filterCoefficients, history).
        passthrough(in, out);   // BYPASS
    }
};

} // namespace hermes::dsp
