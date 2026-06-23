#pragma once
#include <functional>
#include "audio_core/dsp/Node.hpp"

namespace hermes::dsp {

// A Node backed by a lambda — inject pre/post processing WITHOUT authoring a full
// node class. The lambda is captured at setup time (not the RT path); for strict
// zero-alloc RT prefer a real Node subclass, use FnNode for prototyping / light
// cross-cutting work (gain, metering, logging tap).
//
//   chain.insertBefore("AEC", std::make_unique<FnNode>("pre-gain",
//       [](const AudioBuffer& in, AudioBuffer& out){ /* scale in → out */ }));
class FnNode : public Node {
public:
    using Fn = std::function<void(const AudioBuffer&, AudioBuffer&)>;
    FnNode(const char* name, Fn fn) : name_(name), fn_(std::move(fn)) {}
    const char* name() const override { return name_; }
    void process(const AudioBuffer& in, AudioBuffer& out) override { fn_(in, out); }

private:
    const char* name_;
    Fn          fn_;
};

} // namespace hermes::dsp
