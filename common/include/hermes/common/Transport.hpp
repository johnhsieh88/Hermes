#pragma once
#include <cstdio>

// Cross-process transport binding (SDS §14.5.2).
//
// ModuleId IS the address: each process owns ONE inbound POSIX mq named
// "/hermes.mod.<id>"; senders open the peer's by name. No shared pointer
// registry (that is in-process only). mq message priority carries CMsgHead.prio
// so the §13 URGENT/NORMAL/DEFERRED lanes survive across the process boundary.
namespace hermes {

inline void mq_name_for(int moduleId, char out[32]) {
    std::snprintf(out, 32, "/hermes.mod.%d", moduleId);
}

// All control bodies travel INLINE within the mq message (tiny POD). Bulk audio
// never uses the control plane — it goes via PrerollRing (§16) / PipeWire.
} // namespace hermes
