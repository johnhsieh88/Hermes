#include "hermes/common/Catalog.hpp"
#include "hermes/common/EventMap.hpp"
#include "hermes/common/ModuleId.hpp"
#include "hermes/common/MsgBus.hpp"
#include "audio_core/dsp/Chain.hpp"
#include "audio_core/pipewire/ControlBridge.hpp"
#include <unistd.h>

#if HERMES_HAVE_PIPEWIRE
#include "audio_core/pipewire/PwNode.hpp"
#endif

// ABOX (ModuleId 2) — DSP RT island hosted as a PipeWire SPA node (SDS §14.4).
// Control-plane handlers write SharedControl atomics; the RT node (PwNode::process)
// reads them next quantum (§14.8). Graph-touching commands marshal onto the
// PipeWire loop (§14.9). The barge-in DUCK reflex is LOCAL (§13.3), not a command.
namespace hermes {
class AudioCore : public MsgBus, public EventMap<AudioCore> {
public:
    explicit AudioCore(pw::SharedControl& ctl) : ctl_(ctl) {
        Add(_AudioCore::cmd::SET_MODE,       &AudioCore::onSetMode);
        Add(_AudioCore::cmd::START_CAPTURE,  &AudioCore::onStartCapture);
        Add(_AudioCore::cmd::STOP_CAPTURE,   &AudioCore::onStopCapture);
        Add(_AudioCore::cmd::PLAY_TTS,       &AudioCore::onPlayTts);
        Add(_AudioCore::cmd::STOP_TTS,       &AudioCore::onStopTts);
        Add(_AudioCore::cmd::DUCK_PLAYBACK,  &AudioCore::onDuck);
        Add(_AudioCore::cmd::ARM_BARGE_IN,   &AudioCore::onArmBarge);
        Add(_AudioCore::cmd::RESET_PIPELINE, &AudioCore::onReset);
    }
    int ProcessMsg(CMsg* m) override { return Execute(m->hdr.id, m); }
private:
    // Each handler only stores an atomic; the RT node reads it next quantum (§14.8).
    void onSetMode(const CMsg*)      { /* ctl_.engineMode.store(...) */ }
    void onStartCapture(const CMsg*) { ctl_.capturing.store(true); }   // pre-roll + live → CLOUD_CONNECTOR (§16)
    void onStopCapture(const CMsg*)  { ctl_.capturing.store(false); }
    void onPlayTts(const CMsg*)      {}
    void onStopTts(const CMsg*)      {}
    void onDuck(const CMsg*)         { /* ctl_.playbackVolume.store(target) — policy duck only (§13.3) */ }
    void onArmBarge(const CMsg*)     { ctl_.bargeInArmed.store(true); }
    void onReset(const CMsg*)        { ctl_.resetGen.fetch_add(1); }
    pw::SharedControl& ctl_;
};
} // namespace hermes

int main() {
    hermes::pw::SharedControl ctl;            // RT ↔ control atomics (§14.8)
    hermes::dsp::Chain        chain;          // SRC → AEC → Beamform → SES (bypass)
    chain.prepare(48000, 240);

    hermes::AudioCore abox(ctl);
    abox.ConnectMsg(hermes::ModuleId::AUDIO_CORE);

#if HERMES_HAVE_PIPEWIRE
    hermes::pw::PwNode node(chain, ctl);      // SPA filter node hosts the chain
    node.init(48000, 240);
    node.run();                               // blocks in the PipeWire data loop
#else
    (void)chain;                              // no PipeWire on this host — control plane only
    for (;;) pause();
#endif
    return 0;
}
