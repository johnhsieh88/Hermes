#include "audio_core/pipewire/PwStage.hpp"
#include "audio_core/dsp/nodes/SrcNode.hpp"
#include <memory>

// Standalone PipeWire node — SRC (SDS §4.1), 2-ch in → 2-ch out. One node per
// process; links established by the init process / WirePlumber.
int main() {
    hermes::pw::PwClient client("hermes.src");
    if (client.connect() != 0) return 1;
    hermes::pw::PwStage stage(client, "hermes.src", std::make_unique<hermes::dsp::SrcNode>(), 2, 2);
    if (stage.attach(48000, 240) != 0) return 1;
    client.run();
    return 0;
}
