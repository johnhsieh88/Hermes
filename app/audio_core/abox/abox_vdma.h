/* abox_vdma.h — Virtual DMA engine. Abstracts the planar buffer move that a real I2S/PCM
 * DMA controller performs on the SoC. Today the transfer IS a memcpy ("virtual DMA"); the
 * same API can later back a real DMA descriptor ring or an ALSA mmap (SDS §10.6) without
 * touching callers — the buffer pool and nodes just call abox_vdma_xfer(). Carries
 * DMA-channel-like completion stats (transfers/samples) for observability.
 *
 * It also has a NODE form (abox_node vtable) so a vDMA stage can sit in the graph or be
 * linked with other nodes later (e.g. an ingress source or an egress sink node). */
#ifndef HERMES_ABOX_VDMA_H
#define HERMES_ABOX_VDMA_H

#include <stdint.h>
#include "audio_core/abox/abox_node.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t transfers;   /* completed transfers (≈ DMA descriptors retired) */
    uint64_t samples;     /* total samples moved */
} abox_vdma;

void abox_vdma_init(abox_vdma* dma);

/* The core transfer: dst[c] <- src[c], planar, `channels` × `frames` each. THE "DMA move"
 * (a per-channel memcpy today). Skips channels whose src/dst pointer is NULL. */
void abox_vdma_xfer(abox_vdma* dma, float* const* dst, const float* const* src,
                    int channels, int frames);

/* ── Node form (follows the abox_node vtable; composes/links like any DSP node) ──
 *   ABOX_VDMA_IN  : process() copies the bound external SOURCE → the frame (ingress).
 *   ABOX_VDMA_OUT : process() copies the frame → the bound external DEST (egress).
 * Bind the external planar buffer before each process() — its pointers change per quantum. */
typedef enum { ABOX_VDMA_IN = 0, ABOX_VDMA_OUT = 1 } abox_vdma_dir;

abox_node*       abox_vdma_create(abox_vdma_dir dir);
void             abox_vdma_bind(abox_node* n, float** ext_chan, int ext_channels, int frames);
const abox_vdma* abox_vdma_node_stats(abox_node* n);

#ifdef __cplusplus
}
#endif
#endif /* HERMES_ABOX_VDMA_H */
