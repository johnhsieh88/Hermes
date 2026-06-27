/* beamform_node.c — Beamform (§4.2 MVDR/GSC): combine the echo-free channels into one
 * enhanced mono stream. Currently a naïve average (2→1); the steered MVDR/GSC kernel is
 * TODO. Reduces channel count in place — downstream reads chan[0]. */
#include "audio_core/abox/nodes/node_common.h"

static void beam_process(abox_node* n, abox_frame* io) {
    (void)n;
    if (io->channels >= 2 && io->chan[0] && io->chan[1]) {
        for (int i = 0; i < io->frames; ++i)
            io->chan[0][i] = 0.5f * (io->chan[0][i] + io->chan[1][i]);   /* TODO: MVDR/GSC steer */
    }
    io->channels = 1;   /* N → 1 in place */
}

static const abox_node_ops BEAM_OPS = {
    abox_node_default_prepare, abox_node_default_configure, beam_process,
    abox_node_default_reset, abox_node_default_destroy
};

abox_node* abox_beamform_create(void) { return abox_node_alloc(&BEAM_OPS, 0, 2, 1); }
