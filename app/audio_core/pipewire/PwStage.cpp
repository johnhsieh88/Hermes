#include "audio_core/pipewire/PwStage.hpp"
#include "audio_core/pipewire/Pw.hpp"
#include <cstring>

// PwStage binds one dsp::Node to a PipeWire filter node via the Pw.hpp wrapper
// (PwClient + create_pw_node). No pipewire.h here — all PipeWire calls live in
// Pw.cpp. The DSP node bypasses for now, so the stage passes audio through.
namespace hermes::pw {

struct PwStage::Impl {
    const char*                   name;
    std::unique_ptr<dsp::Node>    node;
    int                           chIn  = 0;
    int                           chOut = 0;
    PwClient                      client;
    std::unique_ptr<PwFilterNode> filterNode;
    dsp::AudioBuffer              in{};
    dsp::AudioBuffer              out{};
    explicit Impl(const char* app) : name(app), client(app) {}

    // Per-quantum (static member → can access Impl; usable as a BlockFn pointer):
    // PipeWire hands us mono channel pointers → fill AudioBuffer → node->process()
    // → write output channels.
    static void block(void* user, const float* const* in, int chIn,
                      float* const* out, int chOut, uint32_t n, uint64_t samplePos) {
        auto* d = static_cast<Impl*>(user);
        d->in.channelCount = chIn;
        d->in.sampleCount  = static_cast<int>(n);
        d->in.samplePos    = samplePos;
        for (int c = 0; c < chIn; ++c)
            if (in[c]) std::memcpy(d->in.chan[c], in[c], n * sizeof(float));

        d->node->process(d->in, d->out);

        for (int c = 0; c < chOut; ++c)
            if (out[c]) {
                int src = (c < d->out.channelCount) ? c : 0;
                std::memcpy(out[c], d->out.chan[src], n * sizeof(float));
            }
    }
};

PwStage::PwStage(const char* nodeName, std::unique_ptr<dsp::Node> node, int channelsIn, int channelsOut)
    : impl_(std::make_unique<Impl>(nodeName)) {
    impl_->node  = std::move(node);
    impl_->chIn  = channelsIn;
    impl_->chOut = channelsOut;
}

PwStage::~PwStage() = default;

int PwStage::init(int sampleRate, int quantum) {
    if (impl_->client.connect() != 0) return -1;
    impl_->filterNode = create_pw_node(impl_->client, impl_->name, impl_->chIn, impl_->chOut,
                                       &Impl::block, impl_.get(), sampleRate, quantum);
    return impl_->filterNode ? 0 : -1;
}

void PwStage::run()  { impl_->client.run(); }
void PwStage::quit() { impl_->client.quit(); }

} // namespace hermes::pw
