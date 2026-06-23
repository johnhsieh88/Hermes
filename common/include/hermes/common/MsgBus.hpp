#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <pthread.h>
#include "hermes/common/CMsg.hpp"

// Cross-process control bus (SDS §14.8), API-compatible with the reference
// TradingAlpha MsgBus. Transport = one POSIX mq per ModuleId ("/hermes.mod.<id>",
// SDS §14.5.2) — ModuleId IS the address; there is no in-process pointer registry
// (which cannot route across Hermes's separate processes). A module subclasses
// MsgBus and overrides ProcessMsg() (typically `return Execute(id, m);`).
//
// The RT island never calls this; it crosses the boundary via the lock-free ring
// + atomics (SDS §14.8).
namespace hermes {

class MsgBus {
public:
    MsgBus();
    virtual ~MsgBus();

    // ── Lifecycle ──
    int  ConnectMsg(int sysId, bool recvTask = true);  // open own mq; spawn recv thread
    int  DisconnectMsg();

    // ── Send ──
    virtual int SendMsg(CMsgHead* pMsg, int dest, bool flowctl = false);
    int  TrySendMsgBestEffort(CMsgHead* pMsg, int dest);   // never blocks (drop on full)
    // Convenience: build header (+ inline POD body), then SendMsg(CMsgHead*, dest).
    int  SendMsg(int dest, uint16_t id, TriggerPrio prio = PRIO_NORMAL,
                 const void* body = nullptr, uint32_t len = 0);

    // ── Receive ──
    int  RecvMsg(CMsg* pMsg);                  // blocking read from own mq
    // Recv-thread loop. virtual WITH a default (RecvMsg → ProcessMsg → FlushMsg);
    // a child may override for a custom drain (e.g. priority-lane ordering, batching).
    virtual void RecvMsgTask(void* pArg);
    virtual int ProcessMsg(CMsg* pMsg);        // override → EventMap::Execute(id, msg)
    void FlushMsg(CMsg* pMsg);                 // release a message body

    // ── Queue management / introspection ──
    int    GetQ(int id);
    void   SetQ(int q, int id);
    int    ConfigMsgQ(int qid, int extraSize);
    size_t PendingQueueDepth();                // own inbound depth (mq_curmsgs)
    void   DumpMsg(char* pBuff, int nByte, const char* pTitle);

    // NOTE: no SendSyncMsg/SignalSync — Hermes is fully async (SDS §15/§16). A
    // QUERY_STATE reply returns as an ordinary event; nothing blocks on a reply.

    int  MyId() const { return mMyId; }

protected:
    static void* RecvMsgTaskStatic(void* context);

    int       mMyId;       // this module's ModuleId (set by ConnectMsg)
    int       mMyQ;        // this module's inbound mq descriptor
    int       mWaitMs;
    pthread_t mRecvTask;
    std::atomic<bool> mConnected;

    static constexpr size_t kMaxQueueDepth = 4096;
};

} // namespace hermes
