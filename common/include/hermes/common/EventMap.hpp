#pragma once
#include <cstdint>
#include <map>
#include "hermes/common/CMsg.hpp"

// Per-module dispatch table (SDS §14.7): id -> member-function handler.
// A module = MsgBus (transport) + EventMap<Self> (dispatch).
namespace hermes {

template <class T>
class EventMap {
public:
    using Handler = void (T::*)(const CMsg*);

    void Add(uint16_t id, Handler h) { map_[id] = h; }
    void Erase(uint16_t id)          { map_.erase(id); }

    // Returns 1 if handled, 0 if no handler registered for id.
    int Execute(uint16_t id, const CMsg* m) {
        auto it = map_.find(id);
        if (it == map_.end()) return 0;
        (static_cast<T*>(this)->*(it->second))(m);
        return 1;
    }

private:
    std::map<uint16_t, Handler> map_;
};

} // namespace hermes
