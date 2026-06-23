#include "audio_core/pipewire/PwStage.hpp"
#include "audio_core/dsp/nodes/AecNode.hpp"
#include <memory>

// Standalone PipeWire node — AEC (SDS §4.3). 2-ch mic in → 2-ch out.
// TODO: add a far-end reference input port (post-fader, §4.3.1) wired to
//       AecNode::setReference; the init process links the playback-sink monitor
//       into it. PwStage's generic path currently maps mic ports only.
int main() {
    hermes::pw::PwStage stage("hermes.aec", std::make_unique<hermes::dsp::AecNode>(), 2, 2);
    if (stage.init(48000, 240) == 0) stage.run();
    return 0;
}
