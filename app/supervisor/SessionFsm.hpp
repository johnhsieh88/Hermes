#pragma once
#include <thread>
#include "hermes/common/CMsg.hpp"
#include "hermes/common/EventMap.hpp"
#include "hermes/common/EventQueue.hpp"
#include "hermes/common/MsgBus.hpp"
#include "hermes/common/WorkerPool.hpp"

// High-level dialog lifecycle (SDS §15), owned by SUPERVISOR — the control-logic
// process. It drives the ABOX EngineMode via commands; it never reaches into the
// engine and holds no audio.
//
// Threading (SDS §15.7):
//   ① recv thread (HIGH prio, RecvMsgTask override) — drains the mq, enqueues to
//      the FIFO. Does NO handling.
//   ② FsmLoop (ONE serialized thread) — pops the FIFO, runs transitions in order.
//      Only this thread touches state_  ⇒  race-free, lock-free, no pool.
//   ③ WorkerPool — long Supervisor-local tasks; results posted back as events.
//
// NOTE: SS_WAKE_CONFIRM retired (§16) — VTS does full keyword detection, so
// SS_IDLE → SS_CAPTURE directly on WAKE_CONFIRMED.
namespace hermes {

enum SessionState {
    SS_INIT = 0,
    SS_IDLE,        // keyword listening (VTS armed)
    SS_CAPTURE,     // streaming utterance to CLOUD_CONNECTOR
    SS_THINK,       // awaiting first TTS chunk
    SS_SPEAK,       // TTS playback, barge-in armed
    SS_BARGE_DUCK,  // ducking → restart capture
    SS_FAULT,       // reset + recover
    SS_SHUTDOWN,
    SS_STATE_COUNT
};
const char* SessionStateName(SessionState s);

class Supervisor : public MsgBus, public EventMap<Supervisor> {
public:
    Supervisor();

    void Start();   // spawn the single FSM thread + worker pool (recv thread = ConnectMsg)
    void Stop();

    // ① HIGH-prio intake: drain mq → enqueue to FIFO. NO handling here.
    void RecvMsgTask(void* pArg) override;
    // ② Called by the SINGLE FSM thread for each event (serialized → race-free).
    int  ProcessMsg(CMsg* m) override { return Execute(m->hdr.id, m); }

    // Post a self-event (used by worker-pool results to re-enter the FSM thread).
    void PostSelfEvent(uint16_t id, TriggerPrio prio = PRIO_NORMAL,
                       const void* body = nullptr, uint32_t len = 0);

    SessionState state() const { return state_; }

private:
    void FsmLoop();                      // ② the single serialized FSM consumer

    void enter(SessionState s);          // entry actions (e.g. SS_FAULT reset)
    void startTurn();                    // open stream + start capture

    // transition actions (run on the FSM thread; emit commands via SendMsg)
    void onWake(const CMsg*);            // SS_IDLE       → SS_CAPTURE    (KEY PATH: KWD)
    void onBargeIn(const CMsg*);         // SS_SPEAK      → SS_BARGE_DUCK (KEY PATH: barge-in)
    void onSttEndpoint(const CMsg*);     // SS_CAPTURE    → SS_THINK
    void onTtsChunk(const CMsg*);        // SS_THINK      → SS_SPEAK
    void onTtsStreamEnd(const CMsg*);    // mark end-pending
    void onPlaybackDrained(const CMsg*); // SS_SPEAK      → SS_IDLE
    void onModeChanged(const CMsg*);     // SS_BARGE_DUCK → SS_CAPTURE
    void onFault(const CMsg*);           // *             → SS_FAULT

    EventQueue   eq_;                    // ① recv + ③ worker results → ② single FSM consumer
    WorkerPool   pool_;                  // ③ long Supervisor-local tasks
    std::thread  fsmThread_;             // ② the one thread that owns state_
    SessionState state_    = SS_INIT;
    bool         ttsEnded_ = false;      // two-signal TTS completion (§15.3)
};

} // namespace hermes
