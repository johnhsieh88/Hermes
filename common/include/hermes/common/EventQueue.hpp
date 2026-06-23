#pragma once
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include "hermes/common/CMsg.hpp"

// In-process priority event FIFO for the control plane (SDS §15 threading).
// Multi-producer (high-prio recv thread + worker-pool results) → SINGLE consumer
// (the FSM thread). URGENT lane drained first, then NORMAL, then DEFERRED.
// mutex/condvar are fine here: this is the NON-RT control plane, not the RT island.
namespace hermes {

static constexpr int CMSG_MAX_BODY = 256;

// Self-contained copy (header + inline body) so the FIFO owns its data.
struct StoredMsg {
    CMsgHead      hdr;
    unsigned char body[CMSG_MAX_BODY];
};

class EventQueue {
public:
    void push(const CMsg& m) {
        StoredMsg sm;
        sm.hdr = m.hdr;
        uint32_t n = m.hdr.length < CMSG_MAX_BODY ? m.hdr.length : CMSG_MAX_BODY;
        if (m.pBody && n) std::memcpy(sm.body, m.pBody, n);
        int lane = m.hdr.prio < 3 ? m.hdr.prio : PRIO_NORMAL;
        { std::lock_guard<std::mutex> g(mMutex); mLane[lane].push_back(sm); }
        mCond.notify_one();
    }

    // Blocks until an event or stop(). Returns false once stopped AND drained.
    bool pop(StoredMsg* out) {
        std::unique_lock<std::mutex> lk(mMutex);
        mCond.wait(lk, [&] { return mStop || !empty(); });
        for (int p = 0; p < 3; ++p)
            if (!mLane[p].empty()) { *out = mLane[p].front(); mLane[p].pop_front(); return true; }
        return false;   // stopped & empty
    }

    void stop() { { std::lock_guard<std::mutex> g(mMutex); mStop = true; } mCond.notify_all(); }

private:
    bool empty() const { return mLane[0].empty() && mLane[1].empty() && mLane[2].empty(); }
    std::mutex              mMutex;
    std::condition_variable mCond;
    std::deque<StoredMsg>   mLane[3];   // PRIO_URGENT, PRIO_NORMAL, PRIO_DEFERRED
    bool                    mStop = false;
};

} // namespace hermes
