#include "hermes/common/Catalog.hpp"
#include "hermes/common/EventMap.hpp"
#include "hermes/common/ModuleId.hpp"
#include "hermes/common/MsgBus.hpp"
#include <unistd.h>

// CODEC_HW (ModuleId 6) — I2C codec + /dev/input buttons. Obeys reset/gain/mute;
// emits hardware events (UNPLUGGED/READY/OVERTEMP) and buttons (BUTTON_WAKE/MUTE).
namespace hermes {
class CodecHw : public MsgBus, public EventMap<CodecHw> {
public:
    CodecHw() {
        Add(_CodecHw::cmd::RESET,    &CodecHw::onReset);
        Add(_CodecHw::cmd::SET_GAIN, &CodecHw::onSetGain);
        Add(_CodecHw::cmd::MUTE,     &CodecHw::onMute);
        Add(_CodecHw::cmd::UNMUTE,   &CodecHw::onUnmute);
    }
    int ProcessMsg(CMsg* m) override { return Execute(m->hdr.id, m); }
private:
    void onReset(const CMsg*)   {}  // I2C codec reset (§5.2 long task → worker)
    void onSetGain(const CMsg*) {}
    void onMute(const CMsg*)    {}
    void onUnmute(const CMsg*)  {}
};
} // namespace hermes

int main() {
    hermes::CodecHw hw;
    hw.ConnectMsg(hermes::ModuleId::CODEC_HW);
    // TODO: poll /dev/input → SendMsg(SUPERVISOR, BUTTON_WAKE/UNPLUGGED, ...).
    for (;;) pause();
    return 0;
}
