/* capgate_node.c — CAPGATE (ARCHITECTURE §13.2.1): the capture on/off gate ending the
 * cascade. STRUCTURAL (runs in every mode) because skipping a mask-gated stage means
 * PASSTHROUGH — the opposite of a closed gate. Instead the gate computes its own gain:
 *
 *     g = abox_route_gain(mode, ABOX_ELEM_CAPGATE) × (open ? 1 : 0)
 *
 * so BOTH controls apply: the routing matrix (idle/reset modes force 0 → out_0 is silent
 * when the device shouldn't be listening) and the runtime START/STOP_CAPTURE commands
 * (abox_capgate_set_open, atomic, control thread). The gain change is ramped linearly
 * across one 240-frame block (~5 ms) to avoid clicks on open/close transitions.
 * Runs post-DMX on the mono frame. Default OPEN so bring-up/loopback flows work before
 * any supervisor is running; the FSM closes/opens it per turn. */
#include "audio_core/abox/nodes/node_common.h"
#include <stdatomic.h>

typedef struct {
    _Atomic int   open;     /* runtime command state: 1 = capture allowed */
    /* Last applied gain. ATOMIC because the async buffer pool runs 2 worker threads that
     * can execute process() on this SAME node for two in-flight periods concurrently
     * (buffer_pipeline.c bp_slot_worker) — a plain float here is a data race. Relaxed is
     * enough: the value only ramps between the exact endpoints 0/1, both workers compute
     * the same target from the same inputs, and a lost intermediate merely restarts a
     * ≤5 ms ramp. (Same hazard class exists for any stateful node under the async pool —
     * SRC's phase carry, AEC's mix ramp — tracked in ARCHITECTURE §22.) */
    _Atomic float g;
} capgate_state;

static void capgate_process(abox_node* n, abox_frame* io) {
    capgate_state* s = (capgate_state*)n->state;
    const float target = abox_route_gain(io->mode, ABOX_ELEM_CAPGATE) *
                         (atomic_load_explicit(&s->open, memory_order_acquire) ? 1.0f : 0.0f);
    const float start = atomic_load_explicit(&s->g, memory_order_relaxed);
    if (start == target) {
        if (target == 1.0f) return;                    /* fully open: identity, zero work */
        if (target == 0.0f) {                          /* fully closed: silence the frame */
            for (int c = 0; c < io->channels; ++c)
                if (io->chan[c])
                    for (int i = 0; i < io->frames; ++i) io->chan[c][i] = 0.0f;
            return;
        }
    }
    const float step = (target - start) / (float)io->frames;   /* one-block linear ramp */
    for (int c = 0; c < io->channels; ++c) {
        if (!io->chan[c]) continue;
        float g = start;
        for (int i = 0; i < io->frames; ++i) { g += step; io->chan[c][i] *= g; }
    }
    atomic_store_explicit(&s->g, target, memory_order_relaxed);
}

static void capgate_reset(abox_node* n) {
    capgate_state* s = (capgate_state*)n->state;
    atomic_store_explicit(&s->g, 0.0f, memory_order_relaxed);
}

static const abox_node_ops CAPGATE_OPS = {
    abox_node_default_prepare, abox_node_default_configure, capgate_process,
    capgate_reset, abox_node_default_destroy
};

abox_node* abox_capgate_create(void) {
    abox_node* n = abox_node_alloc(&CAPGATE_OPS, sizeof(capgate_state), 1, 1);
    if (n) {
        n->name = "capgate";
        capgate_state* s = (capgate_state*)n->state;
        atomic_store_explicit(&s->open, 1, memory_order_release);  /* default open */
        atomic_store_explicit(&s->g, 1.0f, memory_order_relaxed);
    }
    return n;
}

void abox_capgate_set_open(abox_node* n, int open) {
    if (n && n->ops == &CAPGATE_OPS && n->state)
        atomic_store_explicit(&((capgate_state*)n->state)->open, open ? 1 : 0,
                              memory_order_release);
}
