#include "audio_core/pipewire/Pw.hpp"

#include <pipewire/pipewire.h>
#include <cstdio>
#include <vector>

// The ONLY file that includes <pipewire/pipewire.h>. Built only when
// libpipewire-0.3 is found. Implements the PwClient connection and the
// PwFilterNode wrapper (SDS §14.3/§14.4).
namespace hermes::pw {

// ───────────────────────── PwClient (loop + context + core) ─────────────────────────
struct PwClient::Impl {
    pw_main_loop* loop = nullptr;
    pw_context*   ctx  = nullptr;
    pw_core*      core = nullptr;
};

PwClient::PwClient(const char* appName) : impl_(std::make_unique<Impl>()) {
    pw_init(nullptr, nullptr);                                       // 1) init the library
    impl_->loop = pw_main_loop_new(nullptr);                        // 2) event loop
    impl_->ctx  = pw_context_new(pw_main_loop_get_loop(impl_->loop), // 3) the CONTEXT
                                 pw_properties_new(PW_KEY_APP_NAME, appName, nullptr), 0);
}

PwClient::~PwClient() {
    if (impl_->core) pw_core_disconnect(impl_->core);
    if (impl_->ctx)  pw_context_destroy(impl_->ctx);
    if (impl_->loop) pw_main_loop_destroy(impl_->loop);
}

int PwClient::connect() {
    impl_->core = pw_context_connect(impl_->ctx, nullptr, 0);       // 4) connect to the daemon → core
    return impl_->core ? 0 : -1;
}

void PwClient::run()  { if (impl_->loop) pw_main_loop_run(impl_->loop); }
void PwClient::quit() { if (impl_->loop) pw_main_loop_quit(impl_->loop); }
pw_main_loop* PwClient::mainLoop() const { return impl_->loop; }
pw_core*      PwClient::core() const     { return impl_->core; }

// ───────────────────────── PwFilterNode (the node) ─────────────────────────
struct PwFilterNode::Impl {
    pw_filter*                filter = nullptr;
    spa_hook                  listener{};
    pw_filter_events          events{};
    int                       chIn = 0, chOut = 0;
    BlockFn                   fn = nullptr;
    void*                     user = nullptr;
    std::vector<void*>        inPort, outPort;     // per-channel mono DSP ports
    std::vector<const float*> inBuf;
    std::vector<float*>       outBuf;
};

// RT data-loop, once per quantum: resolve port buffers, call the user BlockFn.
static void node_on_process(void* data, spa_io_position* pos) {
    auto* d = static_cast<PwFilterNode::Impl*>(data);
    const uint32_t n = pos->clock.duration;
    for (int c = 0; c < d->chIn; ++c)
        d->inBuf[c] = static_cast<const float*>(pw_filter_get_dsp_buffer(d->inPort[c], n));
    for (int c = 0; c < d->chOut; ++c)
        d->outBuf[c] = static_cast<float*>(pw_filter_get_dsp_buffer(d->outPort[c], n));
    d->fn(d->user, d->inBuf.data(), d->chIn, d->outBuf.data(), d->chOut, n, pos->clock.position);
}

PwFilterNode::PwFilterNode(PwClient& client, const char* name, int chIn, int chOut, BlockFn fn, void* user)
    : impl_(std::make_unique<Impl>()) {
    impl_->chIn = chIn; impl_->chOut = chOut; impl_->fn = fn; impl_->user = user;
    impl_->inBuf.resize(static_cast<size_t>(chIn));
    impl_->outBuf.resize(static_cast<size_t>(chOut));

    impl_->filter = pw_filter_new(client.core(), name,
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                          PW_KEY_MEDIA_CATEGORY, "Filter", nullptr));

    impl_->events.version = PW_VERSION_FILTER_EVENTS;
    impl_->events.process = node_on_process;
    pw_filter_add_listener(impl_->filter, &impl_->listener, &impl_->events, impl_.get());
}

PwFilterNode::~PwFilterNode() {
    if (impl_->filter) pw_filter_destroy(impl_->filter);
}

static void* add_dsp_port(pw_filter* f, spa_direction dir, const char* portName, const char* latency) {
    return pw_filter_add_port(
        f, dir, PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(void*),
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                          PW_KEY_PORT_NAME, portName,
                          PW_KEY_NODE_LATENCY, latency, nullptr),
        nullptr, 0);
}

int PwFilterNode::connect(int sampleRate, int quantum) {
    char latency[32];
    std::snprintf(latency, sizeof latency, "%d/%d", quantum, sampleRate);   // lock 5 ms quantum (§14.11)
    char nm[16];
    for (int c = 0; c < impl_->chIn; ++c) {
        std::snprintf(nm, sizeof nm, "in_%d", c);
        impl_->inPort.push_back(add_dsp_port(impl_->filter, PW_DIRECTION_INPUT, nm, latency));
    }
    for (int c = 0; c < impl_->chOut; ++c) {
        std::snprintf(nm, sizeof nm, "out_%d", c);
        impl_->outPort.push_back(add_dsp_port(impl_->filter, PW_DIRECTION_OUTPUT, nm, latency));
    }
    return pw_filter_connect(impl_->filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0);
}

pw_filter* PwFilterNode::filter() const { return impl_->filter; }

// ───────────────────────── factory ─────────────────────────
std::unique_ptr<PwFilterNode> create_pw_node(PwClient& client, const char* name,
                                             int chIn, int chOut, BlockFn fn, void* user,
                                             int sampleRate, int quantum) {
    auto node = std::make_unique<PwFilterNode>(client, name, chIn, chOut, fn, user);
    if (node->connect(sampleRate, quantum) < 0) return nullptr;
    return node;
}

} // namespace hermes::pw
