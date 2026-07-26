/* beamform_node.c — Beamform (§4.2 MVDR/GSC): spatial filter over the echo-free
 * channels. CHANNEL-PRESERVING (2→2, ARCHITECTURE §13.2):
 *   chan[0] = target beam   = (mic0 + delayed_mic1) × 0.5
 *   chan[1] = blocking ref  = (mic0 − delayed_mic1) × 0.5  (GSC noise reference)
 * The 2→1 collapse is DMX's job downstream. Current kernel: Delay-and-Sum (DAS)
 * on a 2-mic 60 mm linear array with fractional-sample interpolation and cross-block
 * continuity via a tail buffer. MVDR/GSC upgrade replaces beam_process only. */
#include "audio_core/abox/nodes/node_common.h"
#include <math.h>
#include <string.h>

#define BEAM_MIC_SPACING_M  0.060f   /* 60 mm default array pitch */
#define BEAM_SPEED_SOUND    343.0f   /* m/s */
#define BEAM_MAX_DELAY      16       /* supports up to ~114 mm spacing @ 48 kHz */

typedef struct {
    int   sample_rate;
    int   delay_int;                  /* integer steering delay (samples, ≥ 0) */
    float delay_frac;                 /* sub-sample fraction in [0, 1) */
    float tail[BEAM_MAX_DELAY + 1];   /* last N+1 samples of chan[1], prev block */
} beam_state;

/* Return chan[1][idx], transparently satisfying negative indices from the tail.
 * tail[BEAM_MAX_DELAY] = chan1[-1], tail[BEAM_MAX_DELAY-1] = chan1[-2], etc. */
static inline float mic1_at(const beam_state* s, const float* mic1, int idx) {
    if (idx < 0) {
        int ti = BEAM_MAX_DELAY + 1 + idx;   /* idx < 0, so ti < BEAM_MAX_DELAY+1 */
        return (ti >= 0) ? s->tail[ti] : 0.0f;
    }
    return mic1[idx];
}

static void beam_prepare(abox_node* n, const abox_config* cfg) {
    beam_state* s    = (beam_state*)n->state;
    s->sample_rate   = cfg ? cfg->sample_rate : 48000;
    float az_deg     = cfg ? (float)cfg->target_azimuth : 0.0f;
    float delay_samp = fabsf(sinf(az_deg * 3.14159265f / 180.0f))
                       * BEAM_MIC_SPACING_M / BEAM_SPEED_SOUND
                       * (float)s->sample_rate;
    s->delay_int  = (int)delay_samp;
    if (s->delay_int >= BEAM_MAX_DELAY) s->delay_int = BEAM_MAX_DELAY - 1;
    s->delay_frac = delay_samp - (float)s->delay_int;
    memset(s->tail, 0, sizeof s->tail);
}

static void beam_reset(abox_node* n) {
    beam_state* s = (beam_state*)n->state;
    memset(s->tail, 0, sizeof s->tail);
}

static void beam_process(abox_node* n, abox_frame* io) {
    beam_state* s   = (beam_state*)n->state;
    const int   N   = io->frames;
    float*      ch0 = io->chan[0];
    float*      ch1 = io->chan[1];
    const int   di  = s->delay_int;
    const float df  = s->delay_frac;

    if (io->channels < 2 || N <= 0 || N > ABOX_MAX_BLOCK) return;

    /* Scratch copy of mic1 before overwriting ch1 in-place. */
    float m1[ABOX_MAX_BLOCK];
    memcpy(m1, ch1, (size_t)N * sizeof(float));

    if (di == 0 && df == 0.0f) {
        for (int i = 0; i < N; ++i) {
            float m0 = ch0[i], d = m1[i];
            ch0[i] = 0.5f * (m0 + d);   /* beam */
            ch1[i] = 0.5f * (m0 - d);   /* blocking ref */
        }
    } else {
        for (int i = 0; i < N; ++i) {
            float a       = mic1_at(s, m1, i - di);
            float b       = mic1_at(s, m1, i - di - 1);
            float delayed = (1.0f - df) * a + df * b;
            float m0      = ch0[i];
            ch0[i] = 0.5f * (m0 + delayed);
            ch1[i] = 0.5f * (m0 - delayed);
        }
        const int tlen = BEAM_MAX_DELAY + 1;
        if (N >= tlen) {
            memcpy(s->tail, m1 + N - tlen, (size_t)tlen * sizeof(float));
        } else {
            memmove(s->tail, s->tail + N, (size_t)(tlen - N) * sizeof(float));
            memcpy(s->tail + tlen - N, m1, (size_t)N * sizeof(float));
        }
    }
    /* channels stays 2 — DMX collapses to mono downstream */
}

static const abox_node_ops BEAM_OPS = {
    beam_prepare, abox_node_default_configure, beam_process,
    beam_reset,   abox_node_default_destroy
};

abox_node* abox_beamform_create(void) {
    abox_node* n = abox_node_alloc(&BEAM_OPS, sizeof(beam_state), 2, 2);
    if (n) n->name = "beamform";
    return n;
}
