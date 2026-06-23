#pragma once
#include <memory>
#include "audio_core/dsp/Node.hpp"
#include "audio_core/pipewire/Pw.hpp"

// Binds one dsp::Node to a PipeWire filter node created on a SHARED PwClient.
// Because the client is shared (not owned), many PwStages can live in ONE process
// → a multi-node PipeWire graph hosted in a single binary (hermes_abox), as well as
// the one-node-per-process form (hermes_pw_*). The caller owns the client and calls
// connect()/run(); PwStage just creates + holds its filter node.
namespace hermes::pw {

class PwStage {
public:
    PwStage(PwClient& client, const char* name, std::unique_ptr<dsp::Node> node,
            int channelsIn, int channelsOut);
    ~PwStage();
    PwStage(const PwStage&) = delete;
    PwStage& operator=(const PwStage&) = delete;

    int attach(int sampleRate, int quantum);   // create + connect this node on the shared client

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hermes::pw
