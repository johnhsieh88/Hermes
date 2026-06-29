/* harness_offline.c — the SDD §5 offline test harness, in C. Drives the DSP chain
 * (src→aec→beamform→ses) over SYNTHETIC planar frames with NO PipeWire daemon, using the
 * in-process abox_graph as a deterministic engine. The same node vtables that run live
 * under PipeWire run here unchanged. Swap the synthetic generator for a .wav reader to
 * replay captures bit-exactly (ABOX_SIMULATION_MODE). */
#include "audio_core/abox/abox_nodes.h"
#include "audio_core/abox/reference_manager.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#define TWO_PI 6.2831853071795864f

static double run_mode(abox_graph* g, abox_ref_manager* ref, abox_mode mode,
                       int block, int sr) {
    float ch0[ABOX_MAX_BLOCK], ch1[ABOX_MAX_BLOCK], farend[ABOX_MAX_BLOCK];
    abox_frame io;
    io.chan[0] = ch0; io.chan[1] = ch1; io.rate = (uint32_t)sr;
    double energy = 0.0;
    const int blocks = 10;

    for (int blk = 0; blk < blocks; ++blk) {
        io.channels   = 2;
        io.frames     = block;
        io.sample_pos = (uint64_t)blk * block;
        io.mode       = mode;
        for (int i = 0; i < block; ++i) {
            const float t   = (float)(blk * block + i) / (float)sr;
            const float mic = 0.2f * sinf(TWO_PI * 440.0f * t);    /* near-end "speech" */
            ch0[i] = mic; ch1[i] = mic;
            farend[i] = 0.1f * sinf(TWO_PI * 1000.0f * t);         /* post-fader far-end */
        }
        abox_ref_write_farend(ref, farend, NULL, block);          /* tap the reference ring */

        abox_graph_tick(g, &io, mode);                            /* mask-gated chain walk */
        for (int i = 0; i < io.frames; ++i)
            energy += (double)io.chan[0][i] * io.chan[0][i];
    }
    return sqrt(energy / (double)(blocks * block));
}

int main(void) {
    abox_config cfg;
    cfg.sample_rate = 48000; cfg.block_size = 240; cfg.mic_channels = 2;
    cfg.aec_partitions = 40; cfg.aec_fft_size = 512; cfg.aec_mu = 0.1f; cfg.aec_leak = 1e-4f;
    cfg.ref_bulk_delay_max = 1536; cfg.beam_taps = 32; cfg.target_azimuth = 0;
    const int sr = cfg.sample_rate, block = cfg.block_size;

    abox_node* src  = abox_node_create("src");
    abox_node* aec  = abox_node_create("aec");
    abox_node* beam = abox_node_create("beamform");
    abox_node* ses  = abox_node_create("ses");
    if (!src || !aec || !beam || !ses) { fprintf(stderr, "node alloc failed\n"); return 1; }
    src->ops->prepare(src, &cfg); aec->ops->prepare(aec, &cfg);
    beam->ops->prepare(beam, &cfg); ses->ops->prepare(ses, &cfg);

    abox_ref_manager ref;
    abox_ref_prepare(&ref, block);
    abox_ref_set_bulk_delay(&ref, 120, 0.0f);   /* seeded transport delay (§4.3.2c) */
    abox_aec_set_ref(aec, &ref);

    abox_graph g;
    abox_graph_init(&g);
    abox_graph_add(&g, src,  ABOX_ELEM_SRC);
    abox_graph_add(&g, aec,  ABOX_ELEM_AEC);
    abox_graph_add(&g, beam, ABOX_ELEM_BEAM);
    abox_graph_add(&g, ses,  ABOX_ELEM_SES);

    printf("Hermes offline harness (C, no PipeWire) — chain src->aec->beam->ses, block=%d @ %d Hz\n",
           block, sr);
    printf("%-16s %-8s %-8s\n", "mode", "mask", "outRMS");
    struct { abox_mode m; const char* label; } modes[] = {
        { ABOX_MODE_KEYWORD_LISTENING, "KeywordListen"  },
        { ABOX_MODE_CONVERSATION,   "Conversation" },
        { ABOX_MODE_BARGE_IN_MUTING,   "BargeInMuting"  },
    };
    for (unsigned k = 0; k < sizeof(modes) / sizeof(modes[0]); ++k) {
        const double rms = run_mode(&g, &ref, modes[k].m, block, sr);
        printf("%-16s 0x%02x   %.5f\n", modes[k].label,
               (unsigned)abox_active_mask(modes[k].m), rms);
    }

    abox_node_destroy(src); abox_node_destroy(aec);
    abox_node_destroy(beam); abox_node_destroy(ses);
    return 0;
}
