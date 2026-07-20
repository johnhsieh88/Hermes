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

    /* Pull this block's time-aligned, post-fader reference (§4.3.2) — groundwork the PBFDAF
     * kernel will consume as its far-end. */
    if (s->ref && io->frames <= ABOX_MAX_BLOCK)
        abox_ref_read_aligned(s->ref, s->ref_aligned, io->frames);

    /* BYPASS until the PBFDAF kernel + DTD land: there is no echo-cancelled signal yet, so
     * the mode-ramped blend would be a (lossy) identity. Pass audio through unchanged —
     * output buffer == input buffer. The ramp toward the active state still advances so the
     * crossfade is ready the moment a real cancelled stream exists. */
    const float target = abox_route_gain(io->mode, ABOX_ELEM_AEC);   /* 0 bypass / 1 cancel */
    const float rate   = 1.0f / (0.010f * (float)(s->sample_rate > 0 ? s->sample_rate : 48000));
    s->mix += (target > s->mix) ? rate * (float)io->frames
                                : (target < s->mix ? -rate * (float)io->frames : 0.0f);
    if (s->mix < 0.0f) s->mix = 0.0f; else if (s->mix > 1.0f) s->mix = 1.0f;
    /* (When PBFDAF is in: io->chan[c][i] = mix*cancelled[i] + (1-mix)*io->chan[c][i].) */
}

static const abox_node_ops AEC_OPS = {
    aec_prepare, abox_node_default_configure, aec_process,
    abox_node_default_reset, abox_node_default_destroy
};

abox_node* abox_aec_create(void) {
    abox_node* n = abox_node_alloc(&AEC_OPS, sizeof(aec_state), 2, 2);
    if (n) n->name = "aec";
    return n;
}

void abox_aec_set_ref(abox_node* aec, abox_ref_manager* ref) {
    if (aec && aec->ops == &AEC_OPS && aec->state)
        ((aec_state*)aec->state)->ref = ref;
}
