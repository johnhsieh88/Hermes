/* beamform_node.c — Beamform (§4.2 MVDR/GSC): spatial filter over the echo-free
 * channels. CHANNEL-PRESERVING (2→2, ARCHITECTURE §13.2): the target kernel writes
 * chan[0] = the steered beam and chan[1] = the blocking-matrix noise reference (GSC),
 * for a possible post-filter; the 2→1 collapse is DMX's job downstream. The MVDR/GSC
 * kernel is TODO, so this BYPASSES bit-exact (frame untouched — pure passthrough). */
#include "audio_core/abox/nodes/node_common.h"

static const abox_node_ops BEAM_OPS = {
    abox_node_default_prepare, abox_node_default_configure, abox_node_default_process,
    abox_node_default_reset, abox_node_default_destroy
};

abox_node* abox_beamform_create(void) {
    abox_node* n = abox_node_alloc(&BEAM_OPS, 0, 2, 2);
    if (n) n->name = "beamform";
    return n;
}
