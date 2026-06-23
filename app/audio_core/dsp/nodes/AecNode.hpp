#pragma once
#include "audio_core/dsp/Node.hpp"

namespace hermes::dsp {

// Acoustic Echo Cancellation (SDS §4.3) — Partitioned-Block Frequency-Domain
// adaptive filter. Cancels the loudspeaker signal (incl. room tail) from the mic
// using the time-aligned, post-fader far-end reference. Frozen by the DTD during
// double-talk.
class AecNode : public Node {
public:
    const char* name() const override { return "AEC"; }

    // Far-end reference (post-fader, delay-aligned, §4.3.1/§4.3.2) supplied out-of-band.
    void setReference(const AudioBuffer* ref) { ref_ = ref; }
    void setAdaptFrozen(bool f) { adaptFrozen_ = f; }

    void process(const AudioBuffer& in, AudioBuffer& out) override {
        // TODO: fwd FFT → partitioned-conv echo estimate → error/adapt (unless frozen) → RES.
        passthrough(in, out);   // BYPASS (no cancellation yet)
    }

private:
    const AudioBuffer* ref_ = nullptr;
    bool adaptFrozen_ = false;
    // TODO: W[AEC_PARTITIONS][AEC_BINS*2], Xfifo, binPower, mu, leak (§4.3)
};

} // namespace hermes::dsp
