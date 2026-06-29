#pragma once
#include <cstdint>
#include "hermes/common/ModuleId.hpp"

// Hermes command/event catalog (SDS §14.6). IDs are stable — append within a
// module's block, never renumber; reserve removed ids.
namespace hermes {
using namespace ModuleId;

// ───────────── SUPERVISOR (1) — orchestrator / Session FSM (§15) ─────────────
namespace _Supervisor {
namespace cmd {
  static constexpr uint16_t SHUTDOWN        = HM_CMD_FIRST(SUPERVISOR) + 0;
  static constexpr uint16_t SET_MODE_POLICY = HM_CMD_FIRST(SUPERVISOR) + 1;
  static constexpr uint16_t START_SESSION   = HM_CMD_FIRST(SUPERVISOR) + 2;  // PTT — skip wake
  static constexpr uint16_t CANCEL_SESSION  = HM_CMD_FIRST(SUPERVISOR) + 3;
  static constexpr uint16_t FACTORY_RESET   = HM_CMD_FIRST(SUPERVISOR) + 4;
  static constexpr uint16_t QUERY_STATE     = HM_CMD_FIRST(SUPERVISOR) + 5;  // sync
}
namespace evt {
  static constexpr uint16_t STATE_CHANGED   = HM_EVT_FIRST(SUPERVISOR) + 0;
  static constexpr uint16_t SESSION_STARTED = HM_EVT_FIRST(SUPERVISOR) + 1;
  static constexpr uint16_t SESSION_ENDED   = HM_EVT_FIRST(SUPERVISOR) + 2;
  static constexpr uint16_t FAULT           = HM_EVT_FIRST(SUPERVISOR) + 3;
  static constexpr uint16_t READY           = HM_EVT_FIRST(SUPERVISOR) + 4;
}
namespace internal {                                          // FSM-private timer events (not on wire)
  static constexpr uint16_t TO_NO_SPEECH    = HM_EVT_FIRST(SUPERVISOR) + 17;
  static constexpr uint16_t TO_RESPONSE     = HM_EVT_FIRST(SUPERVISOR) + 18;
  static constexpr uint16_t TO_SESSION_MAX  = HM_EVT_FIRST(SUPERVISOR) + 19;
  static constexpr uint16_t TO_RESET        = HM_EVT_FIRST(SUPERVISOR) + 20;
}}

// ───────────── AUDIO_CORE / ABOX (2) — DSP RT island ─────────────
namespace _AudioCore {
namespace cmd {
  static constexpr uint16_t SET_MODE        = HM_CMD_FIRST(AUDIO_CORE) + 0;   // EngineMode
  static constexpr uint16_t START_CAPTURE   = HM_CMD_FIRST(AUDIO_CORE) + 1;
  static constexpr uint16_t STOP_CAPTURE    = HM_CMD_FIRST(AUDIO_CORE) + 2;
  static constexpr uint16_t PLAY_TTS        = HM_CMD_FIRST(AUDIO_CORE) + 3;
  static constexpr uint16_t STOP_TTS        = HM_CMD_FIRST(AUDIO_CORE) + 4;
  static constexpr uint16_t DUCK_PLAYBACK   = HM_CMD_FIRST(AUDIO_CORE) + 5;   // barge-in duck
  static constexpr uint16_t SET_VOLUME      = HM_CMD_FIRST(AUDIO_CORE) + 6;
  static constexpr uint16_t FREEZE_ADAPT    = HM_CMD_FIRST(AUDIO_CORE) + 7;   // DTD
  static constexpr uint16_t ARM_BARGE_IN    = HM_CMD_FIRST(AUDIO_CORE) + 8;
  static constexpr uint16_t DISARM_BARGE_IN = HM_CMD_FIRST(AUDIO_CORE) + 9;
  static constexpr uint16_t RESET_PIPELINE  = HM_CMD_FIRST(AUDIO_CORE) + 10;
  static constexpr uint16_t REF_RELOCK      = HM_CMD_FIRST(AUDIO_CORE) + 11;
  static constexpr uint16_t QUERY_STATE     = HM_CMD_FIRST(AUDIO_CORE) + 12;  // sync
}
namespace evt {
  // +0 RESERVED — KWD_CANDIDATE retired; keyword detection lives entirely in VTS (§16.1)
  static constexpr uint16_t BARGE_IN        = HM_EVT_FIRST(AUDIO_CORE) + 1;   // KEY PATH, URGENT lane
  static constexpr uint16_t VAD_SPEECH_ON   = HM_EVT_FIRST(AUDIO_CORE) + 2;
  static constexpr uint16_t VAD_SPEECH_OFF  = HM_EVT_FIRST(AUDIO_CORE) + 3;
  static constexpr uint16_t CAPTURE_STARTED = HM_EVT_FIRST(AUDIO_CORE) + 4;
  static constexpr uint16_t PLAYBACK_STARTED= HM_EVT_FIRST(AUDIO_CORE) + 5;
  static constexpr uint16_t PLAYBACK_DRAINED= HM_EVT_FIRST(AUDIO_CORE) + 6;
  static constexpr uint16_t REF_LOCKED      = HM_EVT_FIRST(AUDIO_CORE) + 7;
  static constexpr uint16_t AEC_ERLE_DROP   = HM_EVT_FIRST(AUDIO_CORE) + 8;
  static constexpr uint16_t SOFT_MUTE       = HM_EVT_FIRST(AUDIO_CORE) + 9;
  static constexpr uint16_t XRUN            = HM_EVT_FIRST(AUDIO_CORE) + 10;
  static constexpr uint16_t MODE_CHANGED    = HM_EVT_FIRST(AUDIO_CORE) + 11;
  static constexpr uint16_t CLOCK_ANCHOR    = HM_EVT_FIRST(AUDIO_CORE) + 12;  // A/V sync
}}

// ───────────── VOICE_TRIGGER / VTS (3) — always-on KWD ─────────────
namespace _VoiceTrigger {
namespace cmd {
  static constexpr uint16_t ARM           = HM_CMD_FIRST(VOICE_TRIGGER) + 0;
  static constexpr uint16_t DISARM        = HM_CMD_FIRST(VOICE_TRIGGER) + 1;
  static constexpr uint16_t SET_THRESHOLD = HM_CMD_FIRST(VOICE_TRIGGER) + 2;
}
namespace evt {
  static constexpr uint16_t WAKE_CONFIRMED = HM_EVT_FIRST(VOICE_TRIGGER) + 0;  // KEY PATH — body: WakeConfirmedBody
  static constexpr uint16_t WAKE_REJECTED  = HM_EVT_FIRST(VOICE_TRIGGER) + 1;
}}

// ───────────── VIDEO_PROC (4) — A/V sync ─────────────
namespace _VideoProc {
namespace cmd {
  static constexpr uint16_t SYNC_ANCHOR = HM_CMD_FIRST(VIDEO_PROC) + 0;
  static constexpr uint16_t START       = HM_CMD_FIRST(VIDEO_PROC) + 1;
  static constexpr uint16_t STOP        = HM_CMD_FIRST(VIDEO_PROC) + 2;
}
namespace evt {
  static constexpr uint16_t SYNC_DRIFT  = HM_EVT_FIRST(VIDEO_PROC) + 0;
  static constexpr uint16_t FRAME_DROP  = HM_EVT_FIRST(VIDEO_PROC) + 1;
}}

// ───────────── LLM_CONNECTOR (5) — on-target STT/LLM/TTS, local-or-cloud LLM ─────────────
namespace _Llm {
namespace cmd {
  static constexpr uint16_t OPEN_STREAM   = HM_CMD_FIRST(LLM_CONNECTOR) + 0;
  static constexpr uint16_t CLOSE_STREAM  = HM_CMD_FIRST(LLM_CONNECTOR) + 1;
  static constexpr uint16_t UTTERANCE_END = HM_CMD_FIRST(LLM_CONNECTOR) + 2;
  static constexpr uint16_t ABORT         = HM_CMD_FIRST(LLM_CONNECTOR) + 3;  // barge-in cancels in-flight
}
namespace evt {
  static constexpr uint16_t CONNECTED      = HM_EVT_FIRST(LLM_CONNECTOR) + 0;
  static constexpr uint16_t DISCONNECTED   = HM_EVT_FIRST(LLM_CONNECTOR) + 1;
  static constexpr uint16_t STT_PARTIAL    = HM_EVT_FIRST(LLM_CONNECTOR) + 2;
  static constexpr uint16_t STT_FINAL      = HM_EVT_FIRST(LLM_CONNECTOR) + 3;
  static constexpr uint16_t STT_ENDPOINT   = HM_EVT_FIRST(LLM_CONNECTOR) + 4;
  static constexpr uint16_t STT_NO_SPEECH  = HM_EVT_FIRST(LLM_CONNECTOR) + 5;
  static constexpr uint16_t LLM_BEGIN      = HM_EVT_FIRST(LLM_CONNECTOR) + 6;
  static constexpr uint16_t TTS_CHUNK      = HM_EVT_FIRST(LLM_CONNECTOR) + 7;
  static constexpr uint16_t TTS_STREAM_END = HM_EVT_FIRST(LLM_CONNECTOR) + 8;
  static constexpr uint16_t LLM_ERROR      = HM_EVT_FIRST(LLM_CONNECTOR) + 9;   // local or cloud
}}

// ───────────── CODEC_HW (6) — hardware / buttons ─────────────
namespace _CodecHw {
namespace cmd {
  static constexpr uint16_t RESET    = HM_CMD_FIRST(CODEC_HW) + 0;
  static constexpr uint16_t SET_GAIN = HM_CMD_FIRST(CODEC_HW) + 1;
  static constexpr uint16_t MUTE     = HM_CMD_FIRST(CODEC_HW) + 2;
  static constexpr uint16_t UNMUTE   = HM_CMD_FIRST(CODEC_HW) + 3;
}
namespace evt {
  static constexpr uint16_t UNPLUGGED   = HM_EVT_FIRST(CODEC_HW) + 0;
  static constexpr uint16_t PLUGGED     = HM_EVT_FIRST(CODEC_HW) + 1;
  static constexpr uint16_t OVERTEMP    = HM_EVT_FIRST(CODEC_HW) + 2;
  static constexpr uint16_t READY       = HM_EVT_FIRST(CODEC_HW) + 3;
  static constexpr uint16_t BUTTON_WAKE = HM_EVT_FIRST(CODEC_HW) + 4;  // PTT / action button
  static constexpr uint16_t BUTTON_MUTE = HM_EVT_FIRST(CODEC_HW) + 5;  // privacy mute
}}

} // namespace hermes
