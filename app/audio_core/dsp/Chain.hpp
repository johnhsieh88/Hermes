#pragma once
#include <memory>
#include <vector>
#include "audio_core/dsp/Node.hpp"

namespace hermes::dsp {

// Ordered DSP chain (SDS §9.1): SRC → AEC → Beamform → SES → clean mono.
// Drives each Node in sequence with ping-pong scratch buffers. For the scaffold
// every node bypasses, so `out` == raw mic channel 0 (passthrough).
class Chain {
public:
    Chain();                                          // builds the default node order
    void prepare(int sampleRate, int blockSize);
    void reset();
    void process(const AudioBuffer& in, AudioBuffer& out);   // raw mic → clean mono

    // ── building / accessing the graph (SDS §10.3, §12) ──
    void  add(std::unique_ptr<Node> n) { nodes_.push_back(std::move(n)); }   // append (postproc at tail)
    Node* at(std::size_t i)   { return i < nodes_.size() ? nodes_[i].get() : nullptr; }
    Node* find(const char* name);                                           // by Node::name(), e.g. "AEC"
    bool  insertBefore(const char* name, std::unique_ptr<Node> n);          // preprocessing for a stage
    bool  insertAfter (const char* name, std::unique_ptr<Node> n);          // postprocessing for a stage
    std::size_t size() const { return nodes_.size(); }

private:
    std::vector<std::unique_ptr<Node>> nodes_;
    AudioBuffer scratchA_, scratchB_;
};

} // namespace hermes::dsp
