/* ses_node.c — SES (§4.4 spectral suppression / dereverb / AGC) on the mono stream.
 * No kernel yet → wires the shared BYPASS process (output buffer == input buffer).
 * Swap in a real ses_process when the spectral-domain kernel lands. */
#include "audio_core/abox/nodes/node_common.h"

static const abox_node_ops SES_OPS = {
    abox_node_default_prepare, abox_node_default_configure, abox_node_default_process,
    abox_node_default_reset, abox_node_default_destroy
};

abox_node* abox_ses_create(void) { return abox_node_alloc(&SES_OPS, 0, 1, 1); }
