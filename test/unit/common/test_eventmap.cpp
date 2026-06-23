#include <gtest/gtest.h>
#include "hermes/common/CMsg.hpp"
#include "hermes/common/Catalog.hpp"
#include "hermes/common/EventMap.hpp"

using namespace hermes;

TEST(CMsg, HeaderIs20Bytes) {
    EXPECT_EQ(sizeof(CMsgHead), 20u);   // wire-stable (SDS §14.5)
}

TEST(Catalog, IdSchemeEncodesOwnerAndKind) {
    // command in low half, event in high half, top byte = ModuleId (SDS §14.5.1)
    EXPECT_LT(_AudioCore::cmd::SET_MODE & 0xFF, 0x80);
    EXPECT_GE(_AudioCore::evt::BARGE_IN & 0xFF, 0x80);
    EXPECT_EQ(_AudioCore::cmd::SET_MODE >> 8, ModuleId::AUDIO_CORE);
    EXPECT_EQ(_VoiceTrigger::evt::WAKE_CONFIRMED >> 8, ModuleId::VOICE_TRIGGER);
}

struct Dummy : EventMap<Dummy> {
    int hits = 0;
    Dummy() { Add(_AudioCore::evt::BARGE_IN, &Dummy::onBarge); }
    void onBarge(const CMsg*) { ++hits; }
};

TEST(EventMap, DispatchesRegisteredAndIgnoresUnknown) {
    Dummy d;
    CMsg m{};
    m.hdr.id = _AudioCore::evt::BARGE_IN;
    EXPECT_EQ(d.Execute(m.hdr.id, &m), 1);
    EXPECT_EQ(d.hits, 1);
    EXPECT_EQ(d.Execute(0xFFFF, &m), 0);   // no handler → 0, no crash
}
