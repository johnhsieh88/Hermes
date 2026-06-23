#pragma once
#include <memory>
#include "audio_core/dsp/Node.hpp"

// Wraps ONE dsp::Node as a STANDALONE PipeWire filter node — a separate graph node,
// individually visible/routable in PipeWire (pw-top, links, WirePlumber policy).
// This is the MULTI-NODE variant of SDS §14.4: each DSP stage is its own PipeWire
// node, vs. the single-node RT island (PwNode) that hosts the whole Chain. Each
// audio channel is one mono PipeWire DSP port.
//
// Built only when libpipewire-0.3 is present (see app/CMakeLists.txt).
namespace hermes::pw {

class PwStage {
public:
    PwStage(const char* nodeName, std::unique_ptr<dsp::Node> node,
            int channelsIn, int channelsOut);
    ~PwStage();
    PwStage(const PwStage&) = delete;
    PwStage& operator=(const PwStage&) = delete;

    int  init(int sampleRate, int quantum);   // pw_init, create filter + per-channel ports
    void run();                               // pw_main_loop_run (blocks)
    void quit();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hermes::pw
