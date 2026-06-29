#pragma once
#include <cstdint>

// Story-agent control bodies. Per the Hermes wire contract (CMsg.hpp), control bodies are tiny
// inline POD — the mq lane caps the inline body at 256 bytes. Dialogue text/speaker/tone are NOT
// sent over the bus; only the segment INDEX crosses it, and the receiver resolves the index to
// the cached script line / pre-rendered audio clip. (Bulk content travels via the cache/SHM.)
namespace hermes {

#pragma pack(push, 4)
struct StorySegmentRef {
    int32_t segment_idx;   // body for _Llm::cmd::PLAY_SEGMENT — which line to render/play
};
#pragma pack(pop)

} // namespace hermes
