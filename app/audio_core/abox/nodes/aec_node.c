/* aec_node.c — AEC (§4.3 PBFDAF): mode-adaptive ramped bypass that pulls the time-
 * aligned, post-fader far-end from the reference manager (§4.3.2). The real partitioned-
 * block frequency-domain echo-cancellation kernel + DTD freeze are TODO; the framework
 * (mode ramp, reference pull, per-channel structure) is in place. */
#include "audio_core/abox/abox_nodes.h"
#include "audio_core/abox/nodes/node_common.h"

typedef struct {
    abox_ref_manager* ref;                       /* aligned post-fader reference source (§4.3.2) */
    float             mix;                        /* 0 = bypass, 1 = cancelling — ramped across blocks */
    int               sample_rate;
    float             ref_aligned[ABOX_MAX_BLOCK];
} aec_state;

static void aec_prepare(abox_node* n, const abox_config* cfg) {
    aec_state* s = (aec_state*)n->state;
    s->sample_rate = cfg ? cfg->sample_rate : 48000;
}

static void aec_process(abox_node* n, abox_frame* io) {
    aec_state* s = (aec_state*)n->state;
    const float target = abox_route_gain(io->mode, ABOX_ELEM_AEC);   /* 0 bypass / 1 cancel */
    const float rate   = 1.0f / (0.010f * (float)(s->sample_rate > 0 ? s->sample_rate : 48000));

    /* Pull this block's time-aligned, post-fader reference (§4.3.2). The PBFDAF below
     * consumes ref_aligned as its far-end. (Real cancellation: TODO PBFDAF + DTD.) */
    if (s->ref && io->frames <= ABOX_MAX_BLOCK)
        abox_ref_read_aligned(s->ref, s->ref_aligned, io->frames);

    float m = s->mix;
    for (int c = 0; c < io->channels; ++c) {
        m = s->mix;
        for (int i = 0; i < io->frames; ++i) {
            m += (target > m) ? rate : (target < m ? -rate : 0.0f);
            if (m < 0.0f) m = 0.0f; else if (m > 1.0f) m = 1.0f;
            const float cancelled = io->chan[c][i];   /* TODO: PBFDAF echo-cancelled sample */
            const float bypass    = io->chan[c][i];
            io->chan[c][i] = m * cancelled + (1.0f - m) * bypass;
        }
    }
    s->mix = (io->channels > 0) ? m : (target > s->mix ? s->mix + rate : s->mix - rate);
}

static const abox_node_ops AEC_OPS = {
    aec_prepare, abox_node_default_configure, aec_process,
    abox_node_default_reset, abox_node_default_destroy
};

abox_node* abox_aec_create(void) { return abox_node_alloc(&AEC_OPS, sizeof(aec_state), 2, 2); }

void abox_aec_set_ref(abox_node* aec, abox_ref_manager* ref) {
    if (aec && aec->ops == &AEC_OPS && aec->state)
        ((aec_state*)aec->state)->ref = ref;
}
