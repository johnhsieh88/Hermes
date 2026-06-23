#include "supervisor/SessionFsm.hpp"
#include "hermes/common/ModuleId.hpp"
#include <unistd.h>

// SUPERVISOR process (ModuleId 1) — the control-logic process (SDS §15).
//   ConnectMsg → spawns the HIGH-prio recv thread (RecvMsgTask override = intake).
//   Start      → spawns the single serialized FSM thread + the worker pool.
int main() {
    hermes::Supervisor sup;
    sup.ConnectMsg(hermes::ModuleId::SUPERVISOR);   // ① high-prio intake → FIFO
    sup.Start();                                    // ② single FSM thread + ③ worker pool
    // TODO: enter(SS_IDLE) on CODEC_HW READY + graph up; wait for SIGTERM → sup.Stop().
    for (;;) pause();
    return 0;
}
