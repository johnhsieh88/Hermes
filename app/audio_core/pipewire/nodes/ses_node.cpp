#include "audio_core/pipewire/PwStage.hpp"
#include "audio_core/dsp/nodes/SesNode.hpp"
#include <memory>

// Standalone PipeWire node — SES / Sound Enhancement (noise suppression / dereverb
// / AGC). 1-ch mono in → 1-ch mono out. Links (Beamform → SES → clean-mono sink /
// VTS / CLOUD_CONNECTOR) are established by the init process.
int main() {
    hermes::pw::PwStage stage("hermes.ses", std::make_unique<hermes::dsp::SesNode>(), 1, 1);
    if (stage.init(48000, 240) == 0) stage.run();
    return 0;
}
