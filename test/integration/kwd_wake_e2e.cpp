#include <gtest/gtest.h>
#include "supervisor/SessionFsm.hpp"

// PRIMARY KPI (SDS §17.2): keyword-wake path.
// TODO: fake VTS writes the pre-roll ring + emits WAKE_CONFIRMED{wake_pos, ...};
//   assert SUPERVISOR: SS_IDLE → SS_CAPTURE, VOICE_TRIGGER::DISARM +
//   CLOUD_CONNECTOR::OPEN_STREAM + AUDIO_CORE::START_CAPTURE emitted, and the
//   pre-roll is lossless (no command clipping).
TEST(KwdWakeE2E, FsmStartsTurnOnWakeConfirmed) {
    GTEST_SKIP() << "TODO: wire SUPERVISOR + fake VTS/pre-roll ring";
}
