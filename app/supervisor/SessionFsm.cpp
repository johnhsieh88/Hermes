#include "supervisor/SessionFsm.hpp"
#include "hermes/common/Catalog.hpp"
#include <pthread.h>
#include <sched.h>

namespace hermes {

// Best-effort RT priority (real target = Linux/Yocto; dev host = no-op).
static void SetSchedFifo(int prio) {
#if defined(__linux__)
    sched_param sp{};
    sp.sched_priority = prio;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
#else
    (void)prio;
#endif
}

const char* SessionStateName(SessionState s) {
    switch (s) {
        case SS_INIT:       return "INIT";
        case SS_IDLE:       return "IDLE";
        case SS_CAPTURE:    return "CAPTURE";
        case SS_THINK:      return "THINK";
        case SS_SPEAK:      return "SPEAK";
        case SS_BARGE_DUCK: return "BARGE_DUCK";
        case SS_FAULT:      return "FAULT";
        case SS_SHUTDOWN:   return "SHUTDOWN";
        default:            return "?";
    }
}

Supervisor::Supervisor() : MsgBus() {
    Add(_VoiceTrigger::evt::WAKE_CONFIRMED,  &Supervisor::onWake);
    Add(_AudioCore::evt::BARGE_IN,           &Supervisor::onBargeIn);
    Add(_Llm::evt::STT_ENDPOINT,           &Supervisor::onSttEndpoint);
    Add(_Llm::evt::TTS_CHUNK,              &Supervisor::onTtsChunk);
    Add(_Llm::evt::TTS_STREAM_END,         &Supervisor::onTtsStreamEnd);
    Add(_AudioCore::evt::PLAYBACK_DRAINED,   &Supervisor::onPlaybackDrained);
    Add(_AudioCore::evt::MODE_CHANGED,       &Supervisor::onModeChanged);
    Add(_CodecHw::evt::UNPLUGGED,            &Supervisor::onFault);
    // TODO: CANCEL_SESSION, SHUTDOWN, timeouts (TO_*), STT_NO_SPEECH → SS_IDLE.
}

// ── ① HIGH-prio intake: drain mq → FIFO, NO handling (SDS §15.7) ──
void Supervisor::RecvMsgTask(void* /*pArg*/) {
    SetSchedFifo(75);                                 // highest among Supervisor threads (< ABOX RT)
    CMsg m;
    while (mConnected.load(std::memory_order_relaxed)) {
        if (RecvMsg(&m) == 0) eq_.push(m);            // copy into FIFO; never process here
    }
}

// ── ② The single serialized FSM consumer — only thread that touches state_ ──
void Supervisor::FsmLoop() {
    SetSchedFifo(70);                                 // elevated, below intake
    StoredMsg sm;
    while (eq_.pop(&sm)) {
        CMsg m{sm.hdr, sm.body};
        ProcessMsg(&m);                               // Execute(id) → transition (µs)
    }
}

void Supervisor::PostSelfEvent(uint16_t id, TriggerPrio prio, const void* body, uint32_t len) {
    unsigned char buf[CMSG_MAX_BODY];
    CMsg m{};
    m.hdr.id = id; m.hdr.prio = static_cast<uint8_t>(prio); m.hdr.length = len;
    if (body && len) { std::memcpy(buf, body, len < CMSG_MAX_BODY ? len : CMSG_MAX_BODY); m.pBody = buf; }
    eq_.push(m);                                       // re-enter the FSM thread
}

void Supervisor::Start() {
    pool_.start(2);                                    // ③ worker pool (SCHED_OTHER)
    fsmThread_ = std::thread([this] { FsmLoop(); });   // ② single FSM thread
}

void Supervisor::Stop() {
    eq_.stop();
    if (fsmThread_.joinable()) fsmThread_.join();
    pool_.stop();
    DisconnectMsg();
}

// ── transition actions (run on the FSM thread) ──
void Supervisor::enter(SessionState s) {
    state_ = s;
    SendMsg(ModuleId::SUPERVISOR, _Supervisor::evt::STATE_CHANGED, PRIO_NORMAL, &s, sizeof s);
    if (s == SS_FAULT) {                               // entry actions (§15.3)
        SendMsg(ModuleId::AUDIO_CORE,      _AudioCore::cmd::RESET_PIPELINE);
        SendMsg(ModuleId::LLM_CONNECTOR, _Llm::cmd::ABORT);
        SendMsg(ModuleId::CODEC_HW,        _CodecHw::cmd::RESET);
    }
}

void Supervisor::startTurn() {
    SendMsg(ModuleId::VOICE_TRIGGER,   _VoiceTrigger::cmd::DISARM);     // no self-wake mid-turn
    SendMsg(ModuleId::LLM_CONNECTOR, _Llm::cmd::OPEN_STREAM);
    SendMsg(ModuleId::AUDIO_CORE,      _AudioCore::cmd::START_CAPTURE); // ABOX: pre-roll ring + live (§16)
    enter(SS_CAPTURE);
}

// KEY PATH: keyword wake (§16.5)
void Supervisor::onWake(const CMsg* /*m*/) {
    if (state_ != SS_IDLE) return;
    startTurn();
}

// KEY PATH: barge-in (§8). The time-critical DUCK is LOCAL in ABOX (§13.3) — the
// Supervisor only does the non-RT orchestration (kill cloud, transition).
void Supervisor::onBargeIn(const CMsg* /*m*/) {
    if (state_ != SS_SPEAK) return;
    SendMsg(ModuleId::LLM_CONNECTOR, _Llm::cmd::ABORT, PRIO_URGENT);   // cancel in-flight TTS/LLM
    enter(SS_BARGE_DUCK);                                                  // STOP_TTS/restart on MODE_CHANGED
}

void Supervisor::onSttEndpoint(const CMsg*) {
    if (state_ != SS_CAPTURE) return;
    SendMsg(ModuleId::LLM_CONNECTOR, _Llm::cmd::UTTERANCE_END);
    SendMsg(ModuleId::AUDIO_CORE,      _AudioCore::cmd::STOP_CAPTURE);
    enter(SS_THINK);
}

void Supervisor::onTtsChunk(const CMsg*) {
    if (state_ != SS_THINK) return;                    // first chunk only
    ttsEnded_ = false;
    SendMsg(ModuleId::AUDIO_CORE, _AudioCore::cmd::PLAY_TTS);
    SendMsg(ModuleId::AUDIO_CORE, _AudioCore::cmd::ARM_BARGE_IN);
    enter(SS_SPEAK);
}

void Supervisor::onTtsStreamEnd(const CMsg*) { ttsEnded_ = true; }   // two-signal completion

void Supervisor::onPlaybackDrained(const CMsg*) {
    if (state_ != SS_SPEAK || !ttsEnded_) return;      // need BOTH signals (§15.3)
    SendMsg(ModuleId::AUDIO_CORE,    _AudioCore::cmd::STOP_TTS);
    SendMsg(ModuleId::AUDIO_CORE,    _AudioCore::cmd::DISARM_BARGE_IN);
    SendMsg(ModuleId::AUDIO_CORE,    _AudioCore::cmd::SET_MODE);        // → KEYWORD_LISTENING
    SendMsg(ModuleId::VOICE_TRIGGER, _VoiceTrigger::cmd::ARM);
    enter(SS_IDLE);
}

void Supervisor::onModeChanged(const CMsg*) {
    if (state_ != SS_BARGE_DUCK) return;               // mute complete → restart capture
    SendMsg(ModuleId::AUDIO_CORE, _AudioCore::cmd::STOP_TTS);
    startTurn();
}

void Supervisor::onFault(const CMsg*) { enter(SS_FAULT); }

} // namespace hermes
