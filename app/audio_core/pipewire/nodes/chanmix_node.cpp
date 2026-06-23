#include "audio_core/pipewire/PwStage.hpp"
#include "audio_core/dsp/nodes/ChannelMixNode.hpp"
#include <cstdlib>
#include <memory>

// Standalone PipeWire node — channel change / matrix-mix (SDS §14.3).
// Usage: hermes_pw_chanmix [inCh] [outCh]   (default 2 → 1, e.g. stereo playback
//        monitor → mono AEC reference). Links are made by the init process.
int main(int argc, char** argv) {
    int inCh  = (argc > 1) ? std::atoi(argv[1]) : 2;
    int outCh = (argc > 2) ? std::atoi(argv[2]) : 1;
    hermes::pw::PwStage stage("hermes.chanmix",
        std::make_unique<hermes::dsp::ChannelMixNode>(outCh), inCh, outCh);
    if (stage.init(48000, 240) == 0) stage.run();
    return 0;
}
