#include "hermes/common/Catalog.hpp"
#include "hermes/common/EventMap.hpp"
#include "hermes/common/ModuleId.hpp"
#include "hermes/common/MsgBus.hpp"
#include <unistd.h>

// VIDEO_PROC (ModuleId 4) — video processing + A/V sync. Aligns frame PTS to the
// audio clock via the CLOCK_ANCHOR events ABOX publishes (SDS §14.10).
namespace hermes {
class VideoProc : public MsgBus, public EventMap<VideoProc> {
public:
    VideoProc() {
        Add(_VideoProc::cmd::SYNC_ANCHOR, &VideoProc::onAnchor);
        Add(_VideoProc::cmd::START,       &VideoProc::onStart);
        Add(_VideoProc::cmd::STOP,        &VideoProc::onStop);
    }
    int ProcessMsg(CMsg* m) override { return Execute(m->hdr.id, m); }
private:
    void onAnchor(const CMsg*) {}  // audio PTS ↔ wallclock map → align video
    void onStart(const CMsg*)  {}
    void onStop(const CMsg*)   {}
};
} // namespace hermes

int main() {
    hermes::VideoProc vp;
    vp.ConnectMsg(hermes::ModuleId::VIDEO_PROC);
    for (;;) pause();
    return 0;
}
