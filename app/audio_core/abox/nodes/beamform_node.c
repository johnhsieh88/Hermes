/* beamform_node.c — Beamform (§4.2 MVDR/GSC): combine the echo-free channels into one
 * enhanced mono stream. The steered MVDR/GSC kernel is TODO, so this BYPASSES: it passes
 * the primary mic (chan[0]) through bit-exact (output buffer == input buffer) and only
 * drops to mono. Swap the spatial combiner in here when it lands. */
#include "audio_core/abox/nodes/node_common.h"

static void beam_process(abox_node* n, abox_frame* io) {
    (void)n;
    io->channels = 1;   /* N → 1: keep chan[0] untouched (no spatial processing yet) */
}

static const abox_node_ops BEAM_OPS = {
    abox_node_default_prepare, abox_node_default_configure, beam_process,
    abox_node_default_reset, abox_node_default_destroy
};

abox_node* abox_beamform_create(void) { return abox_node_alloc(&BEAM_OPS, 0, 2, 1); }
