/* dmx_node.c — DMX (ARCHITECTURE §13.2): the STRUCTURAL 2→1 downmix that ends the
 * capture cascade. Deliberately NOT a routing-matrix column — it is added with
 * ABOX_ELEM_STRUCTURAL and runs in every mode, because out_0 is contractually mono
 * (48 kHz f32) whatever the engine is doing. Today it SELECTS chan[0] (the beam; with
 * a real GSC kernel chan[1] is the blocking-matrix noise reference and stays internal).
 * A weighted downmix would live here if ever needed. Zero state, zero sample writes —
 * pointer/bookkeeping only. */
#include "audio_core/abox/nodes/node_common.h"

static void dmx_process(abox_node* n, abox_frame* io) {
    (void)n;
    io->channels = 1;   /* select chan[0] — no copy; downstream reads one plane */
}

static const abox_node_ops DMX_OPS = {
    abox_node_default_prepare, abox_node_default_configure, dmx_process,
    abox_node_default_reset, abox_node_default_destroy
};

abox_node* abox_dmx_create(void) {
    abox_node* n = abox_node_alloc(&DMX_OPS, 0, 2, 1);
    if (n) n->name = "dmx";
    return n;
}
