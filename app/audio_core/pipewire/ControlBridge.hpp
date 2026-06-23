#pragma once
#include <atomic>
#include <cstdint>

// Control ↔ RT boundary bridge (SDS §14.8). The AudioCore MsgBus handlers (non-RT)
// write these atomics (WORLD → RT); the PipeWire SPA node reads them at the top of
// each process() (RT). RT detections (barge-in, VAD, ERLE, xrun) go the other way
// via a lock-free ring (RT → WORLD) drained by the forwarder → MsgBus. Never a
// mutex on either crossing.
namespace hermes::pw {

struct SharedControl {
    std::atomic<int>      engineMode{0};      // EngineMode (§3)
    std::atomic<float>    playbackVolume{1.f}; // dB fader target (§5.1.3)
    std::atomic<bool>     adaptFrozen{false};  // DTD freeze AEC (§4.3)
    std::atomic<bool>     bargeInArmed{false}; // §8 gating enabled
    std::atomic<bool>     capturing{false};    // route clean mono → CLOUD_CONNECTOR
    std::atomic<uint32_t> resetGen{0};         // bump → RT runs §6.2 reset
    std::atomic<uint64_t> captureFromPos{0};   // pre-roll start (§16.4) on wake
};

} // namespace hermes::pw
