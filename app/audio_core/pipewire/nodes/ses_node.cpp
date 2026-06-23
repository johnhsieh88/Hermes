#include "audio_core/pipewire/PwStage.hpp"
#include "audio_core/dsp/nodes/SesNode.hpp"
#include <memory>

// Standalone PipeWire node — SES / Sound Enhancement (noise suppress / dereverb /
// AGC), 1-ch mono in → 1-ch mono out.
int main() {
    hermes::pw::PwClient client("hermes.ses");
    if (client.connect() != 0) return 1;
    hermes::pw::PwStage stage(client, "hermes.ses", std::make_unique<hermes::dsp::SesNode>(), 1, 1);
    if (stage.attach(48000, 240) != 0) return 1;
    client.run();
    return 0;
}
