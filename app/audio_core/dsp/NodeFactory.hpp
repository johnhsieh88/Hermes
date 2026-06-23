#pragma once
#include <memory>
#include "audio_core/dsp/Node.hpp"

namespace hermes::dsp {

// Data-driven node creation by type name (SDS §12). Returns nullptr for an
// unknown type. Channel counts / wiring are applied by the host (PwStage + the
// init-process PipeWire links).
std::unique_ptr<Node> makeNode(const char* type);

} // namespace hermes::dsp
