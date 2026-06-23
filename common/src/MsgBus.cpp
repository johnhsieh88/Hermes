#include "hermes/common/MsgBus.hpp"
#include "hermes/common/Transport.hpp"
#include <cstring>

// Cross-process bus over POSIX mq (SDS §14.8 / §14.5.2). Each module opens its own
// inbound "/hermes.mod.<id>"; a recv thread drains it and calls ProcessMsg() (which
// a module forwards to EventMap::Execute). Senders open the peer queue by name and
// mq_send with priority = CMsgHead.prio (→ §13 URGENT/NORMAL/DEFERRED lanes).
//
// STUB: the mq_open / mq_send / mq_receive / pthread calls are marked TODO; the API
// surface and control flow match the reference TradingAlpha MsgBus.
namespace hermes {

MsgBus::MsgBus()
    : mMyId(0), mMyQ(-1), mWaitMs(0), mRecvTask(0), mConnected(false) {}

MsgBus::~MsgBus() { DisconnectMsg(); }

// ── Lifecycle ──
int MsgBus::ConnectMsg(int sysId, bool recvTask) {
    mMyId = sysId;
    char name[32];
    mq_name_for(sysId, name);
    // TODO: mq_attr{ .mq_maxmsg=kMaxQueueDepth, .mq_msgsize=... };
    //       mMyQ = mq_open(name, O_RDONLY | O_CREAT | O_NONBLOCK, 0600, &attr);
    mConnected = true;
    if (recvTask) {
        // TODO: pthread_create(&mRecvTask, nullptr, &RecvMsgTaskStatic, this);
    }
    return 0;
}

int MsgBus::DisconnectMsg() {
    if (!mConnected) return 0;
    mConnected = false;
    // TODO: wake + join recv thread; mq_close(mMyQ); mq_unlink(name).
    return 0;
}

// ── Send ──
int MsgBus::SendMsg(CMsgHead* pMsg, int dest, bool /*flowctl*/) {
    if (!pMsg) return -1;
    pMsg->version = PROTOCOL_VERSION;
    pMsg->src     = static_cast<uint8_t>(mMyId);
    pMsg->dest    = static_cast<uint8_t>(dest);
    char name[32];
    mq_name_for(dest, name);
    // TODO: mqd_t d = mq_open(name, O_WRONLY);
    //       mq_send(d, (const char*)pMsg, sizeof(CMsgHead) + pMsg->length, pMsg->prio);
    return 0;
}

int MsgBus::SendMsg(int dest, uint16_t id, TriggerPrio prio, const void* body, uint32_t len) {
    alignas(CMsgHead) unsigned char buf[sizeof(CMsgHead) + 256];
    if (len > sizeof(buf) - sizeof(CMsgHead)) return -1;       // control bodies are tiny POD
    auto* h = reinterpret_cast<CMsgHead*>(buf);
    std::memset(h, 0, sizeof(CMsgHead));
    h->id   = id;
    h->prio = static_cast<uint8_t>(prio);
    h->length = len;
    if (body && len) std::memcpy(buf + sizeof(CMsgHead), body, len);
    return SendMsg(h, dest);
}

int MsgBus::TrySendMsgBestEffort(CMsgHead* pMsg, int dest) {
    // TODO: mq_send with the queue opened O_NONBLOCK; on EAGAIN drop and return -1
    // (a missed best-effort message is harmless — never block the caller).
    return SendMsg(pMsg, dest);
}

// ── Receive ──
int MsgBus::RecvMsg(CMsg* pMsg) {
    if (!pMsg) return -1;
    // TODO: ssize_t n = mq_receive(mMyQ, recvBuf, recvBufSize, &prio);
    //       pMsg->hdr = *(CMsgHead*)recvBuf; pMsg->pBody = recvBuf + sizeof(CMsgHead);
    return -1;   // nothing yet (stub)
}

void MsgBus::RecvMsgTask(void* /*pArg*/) {
    CMsg m{};
    while (mConnected.load(std::memory_order_relaxed)) {
        if (RecvMsg(&m) == 0) {
            ProcessMsg(&m);   // → EventMap::Execute(id, msg) in the module
            FlushMsg(&m);
        }
    }
}

void* MsgBus::RecvMsgTaskStatic(void* context) {
    static_cast<MsgBus*>(context)->RecvMsgTask(nullptr);
    return nullptr;
}

int  MsgBus::ProcessMsg(CMsg* /*pMsg*/) { return 0; }   // overridden by each module
void MsgBus::FlushMsg(CMsg* /*pMsg*/)   {}              // recv-buffer body: nothing to free (stub)

// ── Queue management / introspection ──
int    MsgBus::GetQ(int /*id*/)              { return -1; }
void   MsgBus::SetQ(int /*q*/, int /*id*/)   {}
int    MsgBus::ConfigMsgQ(int /*qid*/, int /*extraSize*/) { return 0; }
size_t MsgBus::PendingQueueDepth()           { return 0; }   // TODO: mq_getattr → mq_curmsgs
void   MsgBus::DumpMsg(char* /*pBuff*/, int /*nByte*/, const char* /*pTitle*/) {}

} // namespace hermes
