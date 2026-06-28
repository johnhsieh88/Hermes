#include "hermes/common/Catalog.hpp"
#include "hermes/common/EventMap.hpp"
#include "hermes/common/ModuleId.hpp"
#include "hermes/common/MsgBus.hpp"
#include "hermes/common/PrerollRing.hpp"
#include <unistd.h>

// VTS (ModuleId 3) — always-on keyword detection, its OWN raw mic tap (SDS §16).
// PipeWire client on the mic source; runs the full §7 KWD (Features → Model →
// Detector); writes the shared pre-roll ring; on a hit emits WAKE_CONFIRMED.
// Fully independent of ABOX.
namespace hermes {
class VoiceTrigger : public MsgBus, public EventMap<VoiceTrigger> {
public:
    VoiceTrigger() {
        Add(_VoiceTrigger::cmd::ARM,           &VoiceTrigger::onArm);
        Add(_VoiceTrigger::cmd::DISARM,        &VoiceTrigger::onDisarm);
        Add(_VoiceTrigger::cmd::SET_THRESHOLD, &VoiceTrigger::onThreshold);
    }
    int ProcessMsg(CMsg* m) override { return Execute(m->hdr.id, m); }

    // Called by the KWD detector on a confirmed wake (sample_pos-tagged, §16.4).
    void EmitWake(uint64_t wakePos, uint64_t fromPos, uint32_t epoch) {
        WakeConfirmedBody b{wakePos, fromPos, epoch};
        SendMsg(ModuleId::SUPERVISOR, _VoiceTrigger::evt::WAKE_CONFIRMED, PRIO_NORMAL, &b, sizeof b);
    }
private:
    void onArm(const CMsg*)       {}  // enable scoring (SS_IDLE)
    void onDisarm(const CMsg*)    {}  // suppress during a turn
    void onThreshold(const CMsg*) {}
};
} // namespace hermes

int main() {
    hermes::VoiceTrigger vts;
    vts.ConnectMsg(hermes::ModuleId::VOICE_TRIGGER);
    // TODO: PipeWire capture (own raw mic tap) → Preroll_Write() + KWD pipeline → EmitWake().
    for (;;) pause();
    return 0;
}
