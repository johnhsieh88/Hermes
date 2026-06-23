#include "audio_core/dsp/NodeFactory.hpp"
#include "audio_core/dsp/nodes/AecNode.hpp"
#include "audio_core/dsp/nodes/BeamformNode.hpp"
#include "audio_core/dsp/nodes/ChannelMixNode.hpp"
#include "audio_core/dsp/nodes/SesNode.hpp"
#include "audio_core/dsp/nodes/SrcNode.hpp"
#include <cstring>

namespace hermes::dsp {

std::unique_ptr<Node> makeNode(const char* type) {
    if (std::strcmp(type, "src")      == 0) return std::make_unique<SrcNode>();
    if (std::strcmp(type, "aec")      == 0) return std::make_unique<AecNode>();
    if (std::strcmp(type, "beamform") == 0) return std::make_unique<BeamformNode>();
    if (std::strcmp(type, "ses")      == 0) return std::make_unique<SesNode>();
    if (std::strcmp(type, "chanmix")  == 0) return std::make_unique<ChannelMixNode>(1);
    return nullptr;
}

} // namespace hermes::dsp
