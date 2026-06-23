#include "audio_core/pipewire/PwStage.hpp"
#include "audio_core/dsp/nodes/AecNode.hpp"
#include <memory>

// Standalone PipeWire node — AEC (SDS §4.3), 2-ch mic in → 2-ch out.
// TODO: add a far-end reference input port (post-fader, §4.3.1) wired to
//       AecNode::setReference; the init process links the playback-sink monitor in.
int main() {
    hermes::pw::PwClient client("hermes.aec");
    if (client.connect() != 0) return 1;
    hermes::pw::PwStage stage(client, "hermes.aec", std::make_unique<hermes::dsp::AecNode>(), 2, 2);
    if (stage.attach(48000, 240) != 0) return 1;
    client.run();
    return 0;
}
