#include "audio_core/dsp/Chain.hpp"
#include "audio_core/dsp/nodes/AecNode.hpp"
#include "audio_core/dsp/nodes/BeamformNode.hpp"
#include "audio_core/dsp/nodes/ChannelMixNode.hpp"
#include "audio_core/dsp/nodes/SesNode.hpp"
#include "audio_core/dsp/nodes/SrcNode.hpp"
#include <cstring>

namespace hermes::dsp {

Node* Chain::find(const char* name) {
    for (auto& n : nodes_)
        if (std::strcmp(n->name(), name) == 0) return n.get();
    return nullptr;
}

bool Chain::insertBefore(const char* name, std::unique_ptr<Node> n) {
    for (std::size_t i = 0; i < nodes_.size(); ++i)
        if (std::strcmp(nodes_[i]->name(), name) == 0) {
            nodes_.insert(nodes_.begin() + static_cast<long>(i), std::move(n));
            return true;
        }
    return false;
}

bool Chain::insertAfter(const char* name, std::unique_ptr<Node> n) {
    for (std::size_t i = 0; i < nodes_.size(); ++i)
        if (std::strcmp(nodes_[i]->name(), name) == 0) {
            nodes_.insert(nodes_.begin() + static_cast<long>(i + 1), std::move(n));
            return true;
        }
    return false;
}

Chain::Chain() {
    // SDS §9.1 signal-chain order. SES (enhancement) runs on the mono output after
    // beamform. (Order is data-driven per §12 — adjust here / via config.)
    add(std::make_unique<SrcNode>());
    add(std::make_unique<AecNode>());
    add(std::make_unique<BeamformNode>());
    add(std::make_unique<SesNode>());
}

void Chain::prepare(int sampleRate, int blockSize) {
    for (auto& n : nodes_) n->prepare(sampleRate, blockSize);
}

void Chain::reset() {
    for (auto& n : nodes_) n->reset();
}

void Chain::process(const AudioBuffer& in, AudioBuffer& out) {
    if (nodes_.empty()) { out = in; return; }
    const AudioBuffer* cur = &in;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        AudioBuffer* nxt = (i + 1 == nodes_.size())   // last stage writes the caller's `out`
                               ? &out
                               : (i % 2 == 0 ? &scratchA_ : &scratchB_);
        nodes_[i]->process(*cur, *nxt);
        cur = nxt;
    }
}

} // namespace hermes::dsp
