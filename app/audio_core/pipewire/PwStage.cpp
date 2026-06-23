#include "audio_core/pipewire/PwStage.hpp"
#include "audio_core/dsp/ModeControl.hpp"
#include <cstring>

// PwStage binds a dsp::Node to a filter node on a SHARED PwClient (Pw.hpp wrapper).
// No pipewire.h here — all PipeWire calls live in Pw.cpp. DSP nodes bypass for now.
namespace hermes::pw {

struct PwStage::Impl {
    PwClient&                     client;
    const char*                   name;
    std::unique_ptr<dsp::Node>    node;
    int                           chIn  = 0;
    int                           chOut = 0;
    const dsp::ModeControl*       mode  = nullptr;   // shared engine-mode (approach B)
    std::unique_ptr<PwFilterNode> pwnode;
    dsp::AudioBuffer              in{};
    dsp::AudioBuffer              out{};
    Impl(PwClient& c, const char* n) : client(c), name(n) {}

    // Per-quantum bridge (static → usable as a BlockFn): PipeWire mono channel
    // pointers → AudioBuffer → node->process() → output channels.
    static void block(void* user, const float* const* in, int chIn,
                      float* const* out, int chOut, uint32_t n, uint64_t samplePos) {
        auto* d = static_cast<Impl*>(user);
        d->in.channelCount = chIn;
        d->in.sampleCount  = static_cast<int>(n);
        d->in.samplePos    = samplePos;
        // Per-cycle coherent mode latch (approach B): every stage derives the same
        // effective mode for this sample position, so a mid-stream change is skew-free.
        d->in.mode = d->mode ? d->mode->modeAt(samplePos) : dsp::EngineMode::KeywordListening;
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

PwStage::PwStage(PwClient& client, const char* name, std::unique_ptr<dsp::Node> node,
                 int channelsIn, int channelsOut, const dsp::ModeControl* mode)
    : impl_(std::make_unique<Impl>(client, name)) {
    impl_->node  = std::move(node);
    impl_->chIn  = channelsIn;
    impl_->chOut = channelsOut;
    impl_->mode  = mode;
}

PwStage::~PwStage() = default;

int PwStage::attach(int sampleRate, int quantum) {
    impl_->pwnode = create_pw_node(impl_->client, impl_->name, impl_->chIn, impl_->chOut,
                                   &Impl::block, impl_.get(), sampleRate, quantum);
    return impl_->pwnode ? 0 : -1;
}

} // namespace hermes::pw
