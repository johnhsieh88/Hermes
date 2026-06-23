#include "audio_core/pipewire/PwStage.hpp"
#include "audio_core/dsp/ModeControl.hpp"
#include "audio_core/dsp/NodeFactory.hpp"
#include "hermes/common/Catalog.hpp"
#include "hermes/common/EventMap.hpp"
#include "hermes/common/ModuleId.hpp"
#include "hermes/common/MsgBus.hpp"
#include <memory>
#include <vector>

// hermes_abox — the AUDIO_CORE process (ModuleId 2): the MULTI-NODE DSP graph
// (src/aec/beamform/ses, each its own pw_filter) PLUS the control module, in ONE
// binary, on ONE PipeWire client/loop. PipeWire links + schedules the nodes
// (init process / WirePlumber); the nodes adapt by engine mode on a static graph
// (approach B) — no re-routing.
namespace hermes {

// Control: drives the shared ModeControl from SET_MODE (approach B). The MsgBus
// recv thread runs this; nodes read the mode per block via PwStage (lock-free).
class AudioCore : public MsgBus, public EventMap<AudioCore> {
public:
    explicit AudioCore(dsp::ModeControl& mode) : mode_(mode) {
        Add(_AudioCore::cmd::SET_MODE,       &AudioCore::onSetMode);
        Add(_AudioCore::cmd::RESET_PIPELINE, &AudioCore::onReset);
        // TODO: START_CAPTURE / DUCK_PLAYBACK / FREEZE_ADAPT / ARM_BARGE_IN → finer controls.
    }
    int ProcessMsg(CMsg* m) override { return Execute(m->hdr.id, m); }

private:
    void onSetMode(const CMsg* m) {
        if (m->pBody && m->hdr.length >= sizeof(int)) {
            auto em = static_cast<dsp::EngineMode>(*static_cast<const int*>(m->pBody));
            mode_.setMode(em, /*atSamplePos=*/0);   // next block; coherent via samplePos
        }
    }
    void onReset(const CMsg*) {}                     // TODO: §6.2 reset pipeline
    dsp::ModeControl& mode_;
};

} // namespace hermes

int main() {
    using namespace hermes;

    pw::PwClient client("hermes.abox");
    if (client.connect() != 0) return 1;

    static dsp::ModeControl mode;            // shared engine mode (approach B)
    AudioCore ctrl(mode);
    ctrl.ConnectMsg(ModuleId::AUDIO_CORE);   // control plane → onSetMode drives `mode`

    // Create each DSP stage as its own pw_filter node (multi-node graph, one process).
    std::vector<std::unique_ptr<pw::PwStage>> stages;
    auto add = [&](const char* pwName, const char* type, int ci, int co) {
        auto s = std::make_unique<pw::PwStage>(client, pwName, dsp::makeNode(type), ci, co, &mode);
        s->attach(48000, 240);
        stages.push_back(std::move(s));
    };
    add("hermes.src",      "src",      2, 2);
    add("hermes.aec",      "aec",      2, 2);
    add("hermes.beamform", "beamform", 2, 1);
    add("hermes.ses",      "ses",      1, 1);

    // Links (mic→src→aec→beamform→ses→…) are created by the init process / WirePlumber.
    // (Optional: a code-defined wireGraph() could call create_pw_link() here instead.)
    client.run();   // PipeWire data-loop; MsgBus recv thread drives `mode` concurrently
    return 0;
}
