#include "audio_core/pipewire/PwStage.hpp"
#include "audio_core/dsp/nodes/BeamformNode.hpp"
#include <memory>

// Standalone PipeWire node — Beamform (SDS §4.2). 2-ch in → 1-ch mono out.
// Links (AEC → Beamform → SES) are established by the init process.
int main() {
    hermes::pw::PwStage stage("hermes.beamform", std::make_unique<hermes::dsp::BeamformNode>(), 2, 1);
    if (stage.init(48000, 240) == 0) stage.run();
    return 0;
}
