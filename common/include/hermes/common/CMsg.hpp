#pragma once
#include <cstdint>

// Hermes control-plane wire message (SDS §14.5). Fixed 32-byte header + body.
// Body storage is inline / malloc / shared-memory per allocType (bulk → SHM).
namespace hermes {

static constexpr uint16_t PROTOCOL_VERSION = 1;

// Priority lanes — map onto POSIX mq priority AND the §13 in-process dispatcher.
enum TriggerPrio : uint8_t { PRIO_URGENT = 0, PRIO_NORMAL = 1, PRIO_DEFERRED = 2 };

#pragma pack(push, 4)
struct CMsgHead {
    uint16_t version;        // PROTOCOL_VERSION
    uint16_t id;             // catalog key (Catalog.hpp) — encodes owner module + cmd/evt
    uint8_t  src;            // ModuleId of sender
    uint8_t  dest;           // ModuleId of recipient
    uint8_t  prio;           // TriggerPrio lane (→ mq priority)
    uint8_t  flags;          // reserved (bulk audio travels via PrerollRing/PipeWire, not here)
    uint32_t length;         // inline body byte length (0 if none)
    uint64_t timestampNs;    // PipeWire clock domain (spa_io_position.clock.nsec)
};
#pragma pack(pop)
static_assert(sizeof(CMsgHead) == 20, "CMsgHead must stay 20 bytes (wire-stable)");

// Body bytes travel INLINE inside the mq message (control msgs are tiny POD).
// No alloc-type tag and no sync-id: bulk audio uses PrerollRing/PipeWire; queries
// reply as async events, not blocking request/reply.
struct CMsg {
    CMsgHead hdr;
    void*    pBody;          // points into the send/recv buffer (inline body)
};

} // namespace hermes
