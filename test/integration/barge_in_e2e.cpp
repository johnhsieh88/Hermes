#include <gtest/gtest.h>
#include "supervisor/SessionFsm.hpp"

// PRIMARY KPI (SDS §17.2): barge-in path.
// TODO: spin SUPERVISOR with fake ABOX/CLOUD_CONNECTOR endpoints; drive
//   SS_IDLE → wake → capture → think → SPEAK, then inject AUDIO_CORE::BARGE_IN.
//   Assert: CLOUD_CONNECTOR::ABORT emitted, FSM → SS_BARGE_DUCK, and (with a real
//   ABOX) duck-start ≤ 1 ms / silence ≤ 12 ms.
TEST(BargeInE2E, FsmReachesBargeDuckOnBargeIn) {
    GTEST_SKIP() << "TODO: wire SUPERVISOR + fake endpoints over the mq transport";
}
