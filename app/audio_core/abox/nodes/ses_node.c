/* ses_node.c — SES (§4.4 spectral suppression / dereverb / AGC) on the mono stream.
 * Identity passthrough stub; the spectral-domain kernel is TODO. */
#include "audio_core/abox/nodes/node_common.h"

static void ses_process(abox_node* n, abox_frame* io) {
    (void)n; (void)io;   /* TODO: spectral noise suppression / dereverb / AGC */
}

static const abox_node_ops SES_OPS = {
    abox_node_default_prepare, abox_node_default_configure, ses_process,
    abox_node_default_reset, abox_node_default_destroy
};

abox_node* abox_ses_create(void) { return abox_node_alloc(&SES_OPS, 0, 1, 1); }
