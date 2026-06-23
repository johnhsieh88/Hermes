#include "audio_core/pipewire/PwStage.hpp"
#include "audio_core/dsp/nodes/BeamformNode.hpp"
#include <memory>

// Standalone PipeWire node — Beamform (SDS §4.2), 2-ch in → 1-ch mono out.
int main() {
    hermes::pw::PwClient client("hermes.beamform");
    if (client.connect() != 0) return 1;
    hermes::pw::PwStage stage(client, "hermes.beamform", std::make_unique<hermes::dsp::BeamformNode>(), 2, 1);
    if (stage.attach(48000, 240) != 0) return 1;
    client.run();
    return 0;
}
