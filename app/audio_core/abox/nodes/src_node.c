/* src_node.c — SRC (§4.1 ASRC): drift-tracking fractional resampler.
 * Removes ADC↔DAC crystal skew BEFORE AEC (§9.1): each output sample reads the input
 * at a fractional position advancing by `ratio` (= input samples per output ≈ 1.0 ±
 * ppm, supplied by the §5 PI loop via abox_src_set_ratio). Linear interpolation; the
 * sub-sample phase and the previous block's tail are carried across blocks so the
 * stream stays continuous. ratio == 1.0 is an exact identity (no quality loss when
 * there is no drift). Reads are clamped at the block edges (a full input FIFO — needed
 * to sustain ratio≠1 indefinitely — pairs with the §10 queue and is the next step). */
#include "audio_core/abox/abox_nodes.h"
#include "audio_core/abox/nodes/node_common.h"
#include <math.h>
#include <string.h>

typedef struct {
    double ratio;                                   /* input samples per output (≈1.0 ± drift) */
    double phase;                                   /* fractional read carry across blocks */
    int    primed;
    float  last[ABOX_MAX_CHANNELS];                 /* previous block's final sample (continuity) */
    float  in[ABOX_MAX_CHANNELS][ABOX_MAX_BLOCK];   /* in-place-safe input snapshot */
} src_state;

static void src_prepare(abox_node* n, const abox_config* cfg) {
    (void)cfg;
    src_state* s = (src_state*)n->state;
    s->ratio = 1.0; s->phase = 0.0; s->primed = 0;
    for (int c = 0; c < ABOX_MAX_CHANNELS; ++c) s->last[c] = 0.0f;
}
static void src_reset(abox_node* n) { src_prepare(n, NULL); }

static void src_process(abox_node* n, abox_frame* io) {
    src_state* s = (src_state*)n->state;
    const int N = io->frames;
    if (N <= 0 || N > ABOX_MAX_BLOCK) return;
    const double ratio = s->ratio;

    /* Fast path: no drift → exact identity, just keep the continuity tail. */
    if (ratio == 1.0 && s->phase == 0.0) {
        for (int c = 0; c < io->channels && c < ABOX_MAX_CHANNELS; ++c) s->last[c] = io->chan[c][N - 1];
        s->primed = 1;
        return;
    }

    for (int c = 0; c < io->channels && c < ABOX_MAX_CHANNELS; ++c)
        memcpy(s->in[c], io->chan[c], sizeof(float) * (size_t)N);   /* snapshot (read survives write) */

    for (int c = 0; c < io->channels && c < ABOX_MAX_CHANNELS; ++c) {
        const float* x    = s->in[c];
        const float  prev = s->primed ? s->last[c] : x[0];          /* x[-1] for cross-block interp */
        for (int outn = 0; outn < N; ++outn) {
            const double pos = s->phase + (double)outn * ratio;     /* read position, x0 at index 0 */
            const int    i   = (int)floor(pos);
            const double f   = pos - (double)i;
            const float  a   = (i     < 0) ? prev : (i     < N ? x[i]     : x[N - 1]);
            const float  b   = (i + 1 < 0) ? prev : (i + 1 < N ? x[i + 1] : x[N - 1]);
            io->chan[c][outn] = (float)((1.0 - f) * a + f * b);     /* linear interpolation */
        }
        s->last[c] = x[N - 1];
    }
    s->phase = (s->phase + (double)N * ratio) - (double)N;          /* carry sub-sample drift */
    s->primed = 1;
}

static const abox_node_ops SRC_OPS = {
    src_prepare, abox_node_default_configure, src_process, src_reset, abox_node_default_destroy
};

abox_node* abox_src_create(void) { return abox_node_alloc(&SRC_OPS, sizeof(src_state), 2, 2); }

void abox_src_set_ratio(abox_node* src, double ratio) {
    if (src && src->ops == &SRC_OPS && src->state) {
        if (ratio < 0.5) ratio = 0.5; else if (ratio > 2.0) ratio = 2.0;   /* sane drift clamp */
        ((src_state*)src->state)->ratio = ratio;
    }
}
