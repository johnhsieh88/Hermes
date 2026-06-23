#include "audio_core/pipewire/PwStage.hpp"
#include "audio_core/dsp/nodes/ChannelMixNode.hpp"
#include <cstdlib>
#include <memory>

// Standalone PipeWire node — channel change / matrix-mix (SDS §14.3).
// Usage: hermes_pw_chanmix [inCh] [outCh]   (default 2 → 1, e.g. stereo→mono ref).
int main(int argc, char** argv) {
    int inCh  = (argc > 1) ? std::atoi(argv[1]) : 2;
    int outCh = (argc > 2) ? std::atoi(argv[2]) : 1;
    hermes::pw::PwClient client("hermes.chanmix");
    if (client.connect() != 0) return 1;
    hermes::pw::PwStage stage(client, "hermes.chanmix",
        std::make_unique<hermes::dsp::ChannelMixNode>(outCh), inCh, outCh);
    if (stage.attach(48000, 240) != 0) return 1;
    client.run();
    return 0;
}
