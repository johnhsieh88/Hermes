#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include "hermes/common/Catalog.hpp"
#include "hermes/common/MsgBus.hpp"
#include "hermes/common/ModuleId.hpp"

using namespace hermes;

namespace {
// A MsgBus whose recv thread just counts and records what arrived.
struct Receiver : MsgBus {
    std::atomic<int>      hits{0};
    std::atomic<uint16_t> lastId{0};
    std::atomic<int>      lastBody{-1};
    int ProcessMsg(CMsg* m) override {
        lastId.store(m->hdr.id);
        if (m->pBody && m->hdr.length >= sizeof(int))
            lastBody.store(*static_cast<const int*>(m->pBody));
        hits.fetch_add(1);
        return 0;
    }
};
} // namespace

// End-to-end: a real mq message crosses from one module's queue to another's, is
// delivered by the recv thread, and the inline POD body survives the hop.
TEST(MsgBusTransport, DeliversAcrossQueuesWithBody) {
#if !defined(__linux__)
    GTEST_SKIP() << "POSIX mq is Linux-only";
#else
    Receiver rx;
    ASSERT_EQ(rx.ConnectMsg(ModuleId::AUDIO_CORE), 0);          // opens queue + spawns recv thread
    MsgBus tx;
    ASSERT_EQ(tx.ConnectMsg(ModuleId::SUPERVISOR, /*recvTask=*/false), 0);

    const int mode = 2;
    ASSERT_EQ(tx.SendMsg(ModuleId::AUDIO_CORE, _AudioCore::cmd::SET_MODE,
                         PRIO_NORMAL, &mode, sizeof(mode)), 0);

    for (int i = 0; i < 100 && rx.hits.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_EQ(rx.hits.load(), 1);
    EXPECT_EQ(rx.lastId.load(), _AudioCore::cmd::SET_MODE);
    EXPECT_EQ(rx.lastBody.load(), mode);

    tx.DisconnectMsg();
    rx.DisconnectMsg();
#endif
}

// The §13 lanes survive the process boundary: mq delivers URGENT before NORMAL
// before DEFERRED regardless of send order.
TEST(MsgBusTransport, PriorityLanesOrderedAcrossMq) {
#if !defined(__linux__)
    GTEST_SKIP() << "POSIX mq is Linux-only";
#else
    MsgBus rx;
    ASSERT_EQ(rx.ConnectMsg(ModuleId::AUDIO_CORE, /*recvTask=*/false), 0);   // drain manually
    MsgBus tx;
    ASSERT_EQ(tx.ConnectMsg(ModuleId::SUPERVISOR, false), 0);

    ASSERT_EQ(tx.SendMsg(ModuleId::AUDIO_CORE, 0xA1, PRIO_DEFERRED), 0);
    ASSERT_EQ(tx.SendMsg(ModuleId::AUDIO_CORE, 0xB2, PRIO_NORMAL), 0);
    ASSERT_EQ(tx.SendMsg(ModuleId::AUDIO_CORE, 0xC3, PRIO_URGENT), 0);

    CMsg m{};
    ASSERT_EQ(rx.RecvMsg(&m), 0); EXPECT_EQ(m.hdr.id, 0xC3);   // URGENT first
    ASSERT_EQ(rx.RecvMsg(&m), 0); EXPECT_EQ(m.hdr.id, 0xB2);   // then NORMAL
    ASSERT_EQ(rx.RecvMsg(&m), 0); EXPECT_EQ(m.hdr.id, 0xA1);   // then DEFERRED

    tx.DisconnectMsg();
    rx.DisconnectMsg();
#endif
}

// Sending to a module with no open queue must fail cleanly, not crash or block.
TEST(MsgBusTransport, SendToAbsentPeerFailsGracefully) {
#if !defined(__linux__)
    GTEST_SKIP() << "POSIX mq is Linux-only";
#else
    MsgBus tx;
    ASSERT_EQ(tx.ConnectMsg(ModuleId::SUPERVISOR, false), 0);
    EXPECT_EQ(tx.SendMsg(ModuleId::CODEC_HW, 0x42, PRIO_NORMAL), -1);   // no receiver up
    tx.DisconnectMsg();
#endif
}
