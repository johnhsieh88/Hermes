#include "audio_core/pipewire/PwNode.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>

// Real PipeWire SPA filter node (SDS §14.3/§14.4). Compiled only when
// libpipewire-0.3 is present. The DSP chain bypasses for now, so the node passes
// mic → clean mono unchanged.
namespace hermes::pw {

struct PwNode::Impl {
    pw_main_loop* loop = nullptr;
    pw_filter*    filter = nullptr;
    // ports: in_mic, in_ref, out_mono (pw_filter_add_port)
};

// on_process: runs in the RT data-loop, once per quantum.
static void on_process(void* userdata, spa_io_position* pos) {
    auto* self = static_cast<PwNode*>(userdata);
    (void)self; (void)pos;
    // TODO:
    //   1) read self->ctl_ atomics (mode/volume/freeze/armed/capturing).
    //   2) dequeue mic + reference buffers; fill dsp::AudioBuffer (samplePos = pos->clock.position).
    //   3) self->chain_.process(in, out);   // BYPASS → out = mic ch0
    //   4) write `out` to the output port; if capturing, it routes to CLOUD_CONNECTOR.
    //   5) deadline check vs quantum budget → soft-mute (§6.1, §14.11).
}

static const pw_filter_events kFilterEvents = {
    PW_VERSION_FILTER_EVENTS,
    /* .destroy = */ nullptr,
    /* .state_changed = */ nullptr,   // → MsgBus evt::XRUN / MODE_CHANGED (§14.9, main-loop)
    /* .io_changed = */ nullptr,
    /* .param_changed = */ nullptr,
    /* .add_buffer = */ nullptr,
    /* .remove_buffer = */ nullptr,
    /* .process = */ on_process,
    /* .drained = */ nullptr,
    /* .command = */ nullptr,
};

PwNode::PwNode(dsp::Chain& chain, SharedControl& ctl)
    : impl_(new Impl), chain_(chain), ctl_(ctl) {}

PwNode::~PwNode() {
    if (impl_->filter) pw_filter_destroy(impl_->filter);
    if (impl_->loop)   pw_main_loop_destroy(impl_->loop);
    delete impl_;
}

int PwNode::init(int sampleRate, int quantum) {
    (void)sampleRate; (void)quantum;
    pw_init(nullptr, nullptr);
    impl_->loop = pw_main_loop_new(nullptr);
    // TODO: pw_filter_new(...); add in_mic / in_ref / out_mono ports; connect with
    //       node.latency = quantum/sampleRate, node.lock-quantum = true (§14.11);
    //       hook kFilterEvents with `this` as userdata.
    (void)kFilterEvents;
    return 0;
}

void PwNode::run()  { if (impl_->loop) pw_main_loop_run(impl_->loop); }
void PwNode::quit() { if (impl_->loop) pw_main_loop_quit(impl_->loop); }

} // namespace hermes::pw
