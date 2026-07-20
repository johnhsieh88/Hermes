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
    _Atomic int open;       /* runtime command state: 1 = capture allowed */
    float       g;          /* last applied gain (ramp start for the next block) */
} capgate_state;

static void capgate_process(abox_node* n, abox_frame* io) {
    capgate_state* s = (capgate_state*)n->state;
    const float target = abox_route_gain(io->mode, ABOX_ELEM_CAPGATE) *
                         (atomic_load_explicit(&s->open, memory_order_acquire) ? 1.0f : 0.0f);
    const float start = s->g;
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
    s->g = target;
}

static void capgate_reset(abox_node* n) {
    capgate_state* s = (capgate_state*)n->state;
    s->g = 0.0f;
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
        s->g = 1.0f;
    }
    return n;
}

void abox_capgate_set_open(abox_node* n, int open) {
    if (n && n->ops == &CAPGATE_OPS && n->state)
        atomic_store_explicit(&((capgate_state*)n->state)->open, open ? 1 : 0,
                              memory_order_release);
}
