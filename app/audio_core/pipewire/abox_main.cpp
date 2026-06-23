#include "audio_core/pipewire/PwStage.hpp"
#include "audio_core/dsp/ModeControl.hpp"
#include "audio_core/dsp/nodes/AecNode.hpp"
#include "audio_core/dsp/nodes/BeamformNode.hpp"
#include "audio_core/dsp/nodes/SesNode.hpp"
#include "audio_core/dsp/nodes/SrcNode.hpp"
#include <memory>
#include <vector>

// Single ABOX binary hosting the MULTI-NODE DSP graph: ONE PipeWire client/loop,
// multiple pw_filter nodes (each still a separate, individually-routable graph node
// — hermes.src / hermes.aec / hermes.beamform / hermes.ses). Links between them are
// made by the init process / WirePlumber, exactly as with the per-stage binaries.
//
// One process, one PipeWire connection, one RT data-loop driving all stages.
int main() {
    using namespace hermes;

    pw::PwClient client("hermes.abox");
    if (client.connect() != 0) return 1;

    // One shared engine-mode for all stages (approach B). The AudioCore MsgBus
    // handler drives it: onSetMode → mode.setMode(newMode, futureSamplePos). Nodes
    // adapt (e.g. AEC active vs bypass) on a static graph — no re-routing.
    static dsp::ModeControl mode;   // static: outlives the RT data-loop

    std::vector<std::unique_ptr<pw::PwStage>> stages;
    auto add = [&](const char* name, std::unique_ptr<dsp::Node> n, int ci, int co) {
        auto s = std::make_unique<pw::PwStage>(client, name, std::move(n), ci, co, &mode);
        s->attach(48000, 240);
        stages.push_back(std::move(s));
    };

    add("hermes.src",      std::make_unique<dsp::SrcNode>(),      2, 2);
    add("hermes.aec",      std::make_unique<dsp::AecNode>(),      2, 2);
    add("hermes.beamform", std::make_unique<dsp::BeamformNode>(), 2, 1);
    add("hermes.ses",      std::make_unique<dsp::SesNode>(),      1, 1);

    // TODO: run the AudioCore MsgBus module here too (shared process) so SET_MODE
    //       drives `mode`; until then the graph stays in KeywordListening.
    client.run();   // one loop drives every node; init process wires the links
    return 0;
}
