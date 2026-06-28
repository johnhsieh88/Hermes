#pragma once

// Hermes module addressing (SDS §14.5.1). Each module owns a 256-id block:
//   low 128  = commands (imperatives TO the module)
//   high 128 = events   (notifications FROM the module)
namespace hermes {
namespace ModuleId {
enum {
    SUPERVISOR      = 1,  // Session FSM orchestrator (SDS §15)
    AUDIO_CORE      = 2,  // ABOX — DSP RT island, PipeWire SPA node (SDS §14.4)
    VOICE_TRIGGER   = 3,  // VTS — always-on keyword detection, own process + own mic tap (SDS §16)
    VIDEO_PROC      = 4,  // A/V sync
    CLOUD_CONNECTOR = 5,  // on-target proxy: PipeWire client <-> network (STT/LLM/TTS)
    CODEC_HW        = 6,  // I2C codec + /dev/input buttons
    GUI_INTERFACE   = 7,  // dev/test web bridge: HTTP UI → control CMsg on the bus (not on device)
};
} // namespace ModuleId
} // namespace hermes

#define HM_CMD_EVT_GAP   0x80
#define HM_CMD_FIRST(m)  ((m) * 0x0100)                    // commands: base .. base+0x7F
#define HM_EVT_FIRST(m)  (HM_CMD_FIRST(m) + HM_CMD_EVT_GAP) // events:   base+0x80 ..
