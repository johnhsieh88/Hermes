#include "hermes/common/MsgBus.hpp"
#include "hermes/common/Transport.hpp"
#include <cstring>

#if defined(__linux__)
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <sys/stat.h>
#endif

// Cross-process bus over POSIX mq (SDS §14.8 / §14.5.2). Each module opens its own
// inbound "/hermes.mod.<id>"; a recv thread drains it and calls ProcessMsg() (which
// a module forwards to EventMap::Execute). Senders open the peer queue by name and
// mq_send with priority derived from CMsgHead.prio (→ §13 URGENT/NORMAL/DEFERRED lanes).
//
// POSIX mq delivers HIGHEST priority first; our lanes run the other way
// (URGENT=0 < NORMAL=1 < DEFERRED=2), so the mapping inverts (URGENT → highest mq prio).
// On non-Linux dev hosts (no <mqueue.h>) the calls compile to no-ops.
namespace hermes {

MsgBus::MsgBus()
    : mMyId(0), mMyQ(-1), mWaitMs(0), mRecvTask(0), mConnected(false) {}

MsgBus::~MsgBus() { DisconnectMsg(); }

#if defined(__linux__)
static unsigned mq_prio_for(uint8_t prio) {
    return (prio <= PRIO_DEFERRED) ? static_cast<unsigned>(PRIO_DEFERRED - prio) : 0u;
}
#endif

// ── Lifecycle ──
int MsgBus::ConnectMsg(int sysId, bool recvTask) {
    mMyId = sysId;
    char name[32];
    mq_name_for(sysId, name);
#if defined(__linux__)
    mq_unlink(name);                       // clear any stale queue from a prior run
    struct mq_attr attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.mq_maxmsg  = kMqMaxMsg;
    attr.mq_msgsize = static_cast<long>(kMqMsgSize);
    mqd_t q = mq_open(name, O_RDONLY | O_CREAT, 0600, &attr);
    if (q == static_cast<mqd_t>(-1)) { mConnected = false; return -1; }
    mMyQ       = static_cast<int>(q);
    mConnected = true;
    if (recvTask && pthread_create(&mRecvTask, nullptr, &RecvMsgTaskStatic, this) == 0)
        mRecvStarted = true;
    return 0;
#else
    (void)recvTask;
    mConnected = true;
    return 0;
#endif
}

int MsgBus::DisconnectMsg() {
    if (!mConnected.exchange(false)) return 0;
#if defined(__linux__)
    if (mRecvStarted) { pthread_join(mRecvTask, nullptr); mRecvStarted = false; }
    if (mMyQ != -1) { mq_close(static_cast<mqd_t>(mMyQ)); mMyQ = -1; }
    char name[32];
    mq_name_for(mMyId, name);
    mq_unlink(name);
#endif
    return 0;
}

// ── Send ──
int MsgBus::SendMsg(CMsgHead* pMsg, int dest, bool flowctl) {
    if (!pMsg) return -1;
    pMsg->version = PROTOCOL_VERSION;
    pMsg->src     = static_cast<uint8_t>(mMyId);
    pMsg->dest    = static_cast<uint8_t>(dest);
#if defined(__linux__)
    const size_t msglen = sizeof(CMsgHead) + pMsg->length;
    if (msglen > kMqMsgSize) return -1;                       // body too large for the lane
    char name[32];
    mq_name_for(dest, name);
    const int oflag = O_WRONLY | (flowctl ? 0 : O_NONBLOCK);  // flowctl → block; else drop-on-full
    mqd_t d = mq_open(name, oflag);
    if (d == static_cast<mqd_t>(-1)) return -1;               // peer not up / queue absent
    const int rc = mq_send(d, reinterpret_cast<const char*>(pMsg), msglen, mq_prio_for(pMsg->prio));
    mq_close(d);
    return rc == 0 ? 0 : -1;
#else
    (void)dest; (void)flowctl;
    return 0;
#endif
}

int MsgBus::SendMsg(int dest, uint16_t id, TriggerPrio prio, const void* body, uint32_t len) {
    alignas(CMsgHead) unsigned char buf[sizeof(CMsgHead) + 256];
    if (len > sizeof(buf) - sizeof(CMsgHead)) return -1;       // control bodies are tiny POD
    auto* h = reinterpret_cast<CMsgHead*>(buf);
    std::memset(h, 0, sizeof(CMsgHead));
    h->id     = id;
    h->prio   = static_cast<uint8_t>(prio);
    h->length = len;
    if (body && len) std::memcpy(buf + sizeof(CMsgHead), body, len);
    return SendMsg(h, dest);
}

int MsgBus::TrySendMsgBestEffort(CMsgHead* pMsg, int dest) {
    return SendMsg(pMsg, dest, /*flowctl=*/false);            // O_NONBLOCK → EAGAIN drops, never blocks
}

// ── Receive ──
int MsgBus::RecvMsg(CMsg* pMsg) {
    if (!pMsg) return -1;
#if defined(__linux__)
    if (mMyQ == -1) return -1;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 200L * 1000L * 1000L;                       // 200 ms → lets the loop poll mConnected
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
    unsigned prio = 0;
    ssize_t n = mq_timedreceive(static_cast<mqd_t>(mMyQ),
                                reinterpret_cast<char*>(mRecvBuf), kMqMsgSize, &prio, &ts);
    if (n < 0 || static_cast<size_t>(n) < sizeof(CMsgHead)) return -1;   // timeout / short read
    std::memcpy(&pMsg->hdr, mRecvBuf, sizeof(CMsgHead));
    pMsg->pBody = (pMsg->hdr.length > 0) ? (mRecvBuf + sizeof(CMsgHead)) : nullptr;
    return 0;
#else
    return -1;
#endif
}

void MsgBus::RecvMsgTask(void* /*pArg*/) {
    CMsg m{};
    while (mConnected.load(std::memory_order_relaxed)) {
        if (RecvMsg(&m) == 0) {
            ProcessMsg(&m);   // → EventMap::Execute(id, msg) in the module
            FlushMsg(&m);     // free the message body
        }
    }
}

void* MsgBus::RecvMsgTaskStatic(void* context) {
    static_cast<MsgBus*>(context)->RecvMsgTask(nullptr);
    return nullptr;
}

int  MsgBus::ProcessMsg(CMsg* /*pMsg*/) { return 0; }   // overridden by each module
void MsgBus::FlushMsg(CMsg* /*pMsg*/)   {}              // body lives in mRecvBuf; nothing to free

// ── Queue management / introspection ──
int    MsgBus::GetQ(int /*id*/)              { return -1; }
void   MsgBus::SetQ(int /*q*/, int /*id*/)   {}
int    MsgBus::ConfigMsgQ(int /*qid*/, int /*extraSize*/) { return 0; }

size_t MsgBus::PendingQueueDepth() {
#if defined(__linux__)
    if (mMyQ == -1) return 0;
    struct mq_attr attr;
    if (mq_getattr(static_cast<mqd_t>(mMyQ), &attr) == 0)
        return static_cast<size_t>(attr.mq_curmsgs);
#endif
    return 0;
}

void   MsgBus::DumpMsg(char* /*pBuff*/, int /*nByte*/, const char* /*pTitle*/) {}

} // namespace hermes
