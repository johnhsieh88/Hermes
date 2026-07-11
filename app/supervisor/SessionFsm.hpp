#pragma once
#include "hermes/common/CMsg.hpp"
#include "hermes/common/EventMap.hpp"
#include "hermes/common/EventQueue.hpp"
#include "hermes/common/MsgBus.hpp"
#include "hermes/common/WorkerPool.hpp"
#include <thread>

// High-level dialog lifecycle (SDS §15), owned by SUPERVISOR. It drives the ABOX
// EngineMode via commands; it never reaches into the engine.
// NOTE: SS_WAKE_CONFIRM retired (§16) — VTS does full keyword detection in-process,
// so SS_IDLE → SS_CAPTURE directly on WAKE_CONFIRMED.
namespace hermes {

enum SessionState {
    SS_INIT = 0,
    SS_IDLE,        // keyword listening (VTS armed)
    SS_CAPTURE,     // streaming utterance to LLM_CONNECTOR
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
    int ProcessMsg(CMsg* m) override { return Execute(m->hdr.id, m); }
    SessionState state() const { return state_; }

    void Start();                        // ② FSM thread + ③ worker pool
    void Stop();                         // drain + join + disconnect

private:
    void RecvMsgTask(void* pArg) override;   // ① HIGH-prio intake: drain mq → FIFO
    void FsmLoop();                          // ② single serialized FSM consumer
    void PostSelfEvent(uint16_t id, TriggerPrio prio, const void* body, uint32_t len);

    void enter(SessionState s);          // runs entry actions (e.g. SS_FAULT reset)
    void startTurn();                    // common: open stream + start capture

    // transition actions (emit commands via Send)
    void onReady(const CMsg*);           // SS_INIT        → SS_IDLE        (graph up)
    void onWake(const CMsg*);            // SS_IDLE        → SS_CAPTURE     (KEY PATH: KWD)
    void onBargeIn(const CMsg*);         // SS_SPEAK       → SS_BARGE_DUCK  (KEY PATH: barge-in)
    void onSttEndpoint(const CMsg*);     // SS_CAPTURE     → SS_THINK
    void onSttNoSpeech(const CMsg*);     // SS_THINK       → SS_IDLE  (empty transcript)
    void onCloudError(const CMsg*);      // SS_THINK       → SS_IDLE  (LLM/TTS fail)
    void onTtsChunk(const CMsg*);        // SS_THINK       → SS_SPEAK
    void onTtsStreamEnd(const CMsg*);    // mark end-pending
    void onPlaybackDrained(const CMsg*); // SS_SPEAK       → SS_IDLE
    void onModeChanged(const CMsg*);     // SS_BARGE_DUCK  → SS_CAPTURE
    void onFault(const CMsg*);           // *              → SS_FAULT

    EventQueue   eq_;                    // multi-producer (intake + pool) → single FSM consumer
    WorkerPool   pool_;                  // ③ non-RT Supervisor-local tasks
    std::thread  fsmThread_;             // ② the one thread that touches state_
    SessionState state_ = SS_INIT;
    bool ttsEnded_ = false;              // two-signal TTS completion (§15.3)
};

} // namespace hermes
