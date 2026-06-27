/* abox_vdma.c — virtual DMA engine + node form. The transfer is a memcpy today; swap the
 * body of abox_vdma_xfer for a real DMA descriptor ring / ALSA mmap and every caller (the
 * buffer pool, any vDMA node) is upgraded with no signature change. */
#include "audio_core/abox/abox_vdma.h"
#include "audio_core/abox/nodes/node_common.h"
#include <string.h>

void abox_vdma_init(abox_vdma* dma) {
    if (dma) { dma->transfers = 0; dma->samples = 0; }
}

void abox_vdma_xfer(abox_vdma* dma, float* const* dst, const float* const* src,
                    int channels, int frames) {
    if (channels > ABOX_MAX_CHANNELS) channels = ABOX_MAX_CHANNELS;
    if (frames   > ABOX_MAX_BLOCK)    frames   = ABOX_MAX_BLOCK;
    int moved = 0;
    for (int c = 0; c < channels; ++c)
        if (dst && src && dst[c] && src[c]) {
            memcpy(dst[c], src[c], sizeof(float) * (size_t)frames);   /* ← the virtual DMA move */
            ++moved;
        }
    if (dma) {
        dma->transfers += 1;
        dma->samples   += (uint64_t)moved * (uint64_t)frames;
    }
}

/* ── node form ──────────────────────────────────────────────────────────────── */
typedef struct {
    abox_vdma     dma;
    abox_vdma_dir dir;
    float**       ext;       /* bound external planar buffer (capture/playback) */
    int           ext_ch;
    int           frames;    /* 0 → use the frame's own count */
} vdma_state;

static void vdma_prepare(abox_node* n, const abox_config* cfg) {
    (void)cfg;
    vdma_state* s = (vdma_state*)n->state;
    abox_vdma_init(&s->dma);
    s->ext = NULL; s->ext_ch = 0; s->frames = 0;
}

static void vdma_process(abox_node* n, abox_frame* io) {
    vdma_state* s = (vdma_state*)n->state;
    if (!s->ext) return;                                  /* nothing bound this block */
    const int frames = (s->frames > 0) ? s->frames : io->frames;
    if (s->dir == ABOX_VDMA_IN) {
        abox_vdma_xfer(&s->dma, io->chan, (const float* const*)s->ext, s->ext_ch, frames);  /* ext → frame */
        io->channels = s->ext_ch;
        io->frames   = frames;
    } else {
        const int ch = (io->channels < s->ext_ch) ? io->channels : s->ext_ch;
        abox_vdma_xfer(&s->dma, s->ext, (const float* const*)io->chan, ch, frames);          /* frame → ext */
    }
}

static const abox_node_ops VDMA_OPS = {
    vdma_prepare, abox_node_default_configure, vdma_process,
    abox_node_default_reset, abox_node_default_destroy
};

abox_node* abox_vdma_create(abox_vdma_dir dir) {
    /* IN: external source → frame (0 in-ports, N out); OUT: frame → external (N in, 0 out). */
    abox_node* n = abox_node_alloc(&VDMA_OPS, sizeof(vdma_state),
                                   dir == ABOX_VDMA_IN ? 0 : ABOX_MAX_CHANNELS,
                                   dir == ABOX_VDMA_IN ? ABOX_MAX_CHANNELS : 0);
    if (n) ((vdma_state*)n->state)->dir = dir;
    return n;
}

void abox_vdma_bind(abox_node* n, float** ext_chan, int ext_channels, int frames) {
    if (n && n->ops == &VDMA_OPS && n->state) {
        vdma_state* s = (vdma_state*)n->state;
        s->ext = ext_chan; s->ext_ch = ext_channels; s->frames = frames;
    }
}

const abox_vdma* abox_vdma_node_stats(abox_node* n) {
    return (n && n->ops == &VDMA_OPS && n->state) ? &((vdma_state*)n->state)->dma : NULL;
}
