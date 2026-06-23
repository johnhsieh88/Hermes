#include "audio_core/pipewire/PwStage.hpp"
#include "audio_core/dsp/nodes/SrcNode.hpp"
#include <memory>

// Standalone PipeWire node — SRC (SDS §4.1). 2-ch in → 2-ch out (per-channel SRC).
// Ports only; links (e.g. ALSA-source → SRC → AEC) are established by the init
// process / WirePlumber policy.
int main() {
    hermes::pw::PwStage stage("hermes.src", std::make_unique<hermes::dsp::SrcNode>(), 2, 2);
    if (stage.init(48000, 240) == 0) stage.run();
    return 0;
}
