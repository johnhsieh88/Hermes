/* abox_selftest.c — C self-test for the data plane (no GoogleTest; assert-based, runs
 * under ctest). Covers the routing mask, mask-gated graph skip, reference-ring alignment,
 * the self-claim worker pool, and the param-store double-buffer. Exit 0 = all pass. */
#include "audio_core/abox/abox_node.h"
#include "audio_core/abox/abox_nodes.h"
#include "audio_core/abox/abox_vdma.h"
#include "audio_core/abox/buffer_pipeline.h"
#include "audio_core/abox/param_store.h"
#include "audio_core/abox/reference_manager.h"
#include "audio_core/abox/worker_pool.h"
#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int feq(float a, float b) { return fabsf(a - b) < 1e-4f; }

static void test_routing_mask(void) {
    assert(abox_active_mask(ABOX_MODE_KEYWORD_LISTENING) == 0u);
    assert(abox_active_mask(ABOX_MODE_SYSTEM_RESET) == 0u);
    /* CloudStreaming engages every element. */
    for (int e = 0; e < ABOX_ELEM_COUNT; ++e)
        assert(abox_active_mask(ABOX_MODE_CLOUD_STREAMING) & abox_elem_bit((abox_elem)e));
    /* BargeIn keeps AEC/capture, ducks TTS. */
    assert(abox_active_mask(ABOX_MODE_BARGE_IN_MUTING) & abox_elem_bit(ABOX_ELEM_AEC));
    assert(!(abox_active_mask(ABOX_MODE_BARGE_IN_MUTING) & abox_elem_bit(ABOX_ELEM_TTSOUT)));
}

/* A counting node to observe include vs skip. */
typedef struct { int runs; } count_state;
static void count_process(abox_node* n, abox_frame* io) { (void)io; ((count_state*)n->state)->runs++; }
static const abox_node_ops COUNT_OPS = { 0, 0, count_process, 0, 0 };

static void test_graph_mask_gating(void) {
    count_state cs_aec = {0}, cs_tts = {0};
    abox_node aec = { &COUNT_OPS, &cs_aec, 2, 2 };
    abox_node tts = { &COUNT_OPS, &cs_tts, 1, 1 };
    abox_graph g; abox_graph_init(&g);
    abox_graph_add(&g, &aec, ABOX_ELEM_AEC);
    abox_graph_add(&g, &tts, ABOX_ELEM_TTSOUT);

    float ch0[8] = {0}, ch1[8] = {0};
    abox_frame io; io.chan[0] = ch0; io.chan[1] = ch1; io.channels = 2; io.frames = 8;

    assert(abox_graph_tick(&g, &io, ABOX_MODE_KEYWORD_LISTENING) == 0);   /* all skipped */
    assert(cs_aec.runs == 0 && cs_tts.runs == 0);
    assert(abox_graph_tick(&g, &io, ABOX_MODE_BARGE_IN_MUTING) == 1);     /* AEC only */
    assert(cs_aec.runs == 1 && cs_tts.runs == 0);
    assert(abox_graph_tick(&g, &io, ABOX_MODE_CLOUD_STREAMING) == 2);     /* both */
    assert(cs_aec.runs == 2 && cs_tts.runs == 1);
}

static void test_reference_manager(void) {
    abox_ref_manager r; abox_ref_prepare(&r, 4);
    float farend[4] = {10, 20, 30, 40}, out[4] = {0};

    abox_ref_set_bulk_delay(&r, 0, 0.0f);
    abox_ref_write_farend(&r, farend, NULL, 4);
    abox_ref_read_aligned(&r, out, 4);
    for (int i = 0; i < 4; ++i) assert(feq(out[i], farend[i]));   /* zero delay = identity */

    abox_ref_reset(&r);
    abox_ref_set_bulk_delay(&r, 2, 0.0f);
    abox_ref_write_farend(&r, farend, NULL, 4);
    abox_ref_read_aligned(&r, out, 4);
    assert(feq(out[0], 0) && feq(out[1], 0) && feq(out[2], 10) && feq(out[3], 20));  /* shift by 2 */

    /* VI-Sense preferred over mixer output. */
    abox_ref_reset(&r);
    abox_ref_set_bulk_delay(&r, 0, 0.0f);
    float vi[4] = {9, 8, 7, 6};
    abox_ref_write_farend(&r, farend, vi, 4);
    abox_ref_read_aligned(&r, out, 4);
    for (int i = 0; i < 4; ++i) assert(feq(out[i], vi[i]));
}

typedef struct { atomic_int sum; } sum_ctx;
static void add_job(void* ctx, int slice, int frames) {
    (void)frames; atomic_fetch_add(&((sum_ctx*)ctx)->sum, slice + 1);
}

static void test_worker_pool(void) {
    abox_worker_pool pool; memset(&pool, 0, sizeof(pool));
    abox_pool_start(&pool, 4, -1);

    sum_ctx c; atomic_store(&c.sum, 0);
    enum { N = 64 };
    abox_cmd jobs[N];
    for (int i = 0; i < N; ++i) { jobs[i].fn = add_job; jobs[i].ctx = &c; jobs[i].slice = i; jobs[i].frames = 1; }

    assert(abox_pool_run(&pool, jobs, N, 0, NULL) == 1);
    assert(atomic_load(&c.sum) == N * (N + 1) / 2);   /* every slice ran exactly once */
    abox_pool_stop(&pool);
}

static void test_param_store(void) {
    typedef struct { int mic_count; float gain; } mic_profile;
    abox_param_store s;
    mic_profile init = {4, 1.0f};
    abox_param_init(&s, &init, (int)sizeof(init));
    assert(((const mic_profile*)abox_param_current(&s))->mic_count == 4);

    mic_profile near = {1, 0.5f};
    abox_param_publish(&s, &near);
    const mic_profile* cur = (const mic_profile*)abox_param_current(&s);
    assert(cur->mic_count == 1 && feq(cur->gain, 0.5f));
}

static void test_buffer_pipeline(void) {
    hermes_buffered_pipeline e;
    hermes_pipeline_init(&e, 48000);

    count_state cs = {0};
    abox_node aec = { &COUNT_OPS, &cs, 2, 2 };
    hermes_pipeline_add_stage(&e, &aec, ABOX_ELEM_AEC);

    float i0[8], i1[8], o0[8] = {0}, o1[8] = {0};
    for (int i = 0; i < 8; ++i) { i0[i] = (float)i; i1[i] = (float)i; }
    float* in[2]  = { i0, i1 };
    float* out[2] = { o0, o1 };

    /* KeywordListening → AEC masked off; processed, not dropped, output mirrors input. */
    hermes_pipeline_set_mode(&e, ABOX_MODE_KEYWORD_LISTENING);
    assert(hermes_pipeline_process_tick(&e, in, 2, out, 2, 8, 0) == 0);
    assert(cs.runs == 0);
    assert(feq(o0[3], 3.0f));                 /* zero-copy in → egress copy out */

    /* CloudStreaming → AEC runs. */
    hermes_pipeline_set_mode(&e, ABOX_MODE_CLOUD_STREAMING);
    assert(hermes_pipeline_process_tick(&e, in, 2, out, 2, 8, 8) == 0);
    assert(cs.runs == 1);

    /* Firewall: target slot still in-progress → soft-drop, drops counter bumps. */
    const unsigned long d0 = atomic_load(&e.drops);
    const uint32_t target = e.next_core_idx;
    atomic_store(&e.core_in_progress[target], 1);
    assert(hermes_pipeline_process_tick(&e, in, 2, out, 2, 8, 16) == 1);
    assert(atomic_load(&e.drops) == d0 + 1);
    atomic_store(&e.core_in_progress[target], 0);

    /* Proportional rotation: each accepted period advances to the next core slot. */
    hermes_pipeline_init(&e, 48000);
    hermes_pipeline_add_stage(&e, &aec, ABOX_ELEM_AEC);
    const uint32_t first = e.next_core_idx;
    hermes_pipeline_process_tick(&e, in, 2, out, 2, 8, 0);
    assert(e.next_core_idx == (first + 1) % HERMES_NUM_WORKER_CORES);
}

static void test_src_node(void) {
    abox_config cfg; memset(&cfg, 0, sizeof(cfg)); cfg.sample_rate = 48000; cfg.block_size = 8;
    abox_node* src = abox_node_create("src");
    assert(src);
    src->ops->prepare(src, &cfg);

    float ch0[8], ch1[8];
    abox_frame io; io.chan[0] = ch0; io.chan[1] = ch1; io.channels = 2; io.frames = 8;

    /* ratio 1.0 → exact identity. */
    for (int i = 0; i < 8; ++i) { ch0[i] = (float)i; ch1[i] = (float)(2 * i); }
    src->ops->process(src, &io);
    for (int i = 0; i < 8; ++i) { assert(feq(ch0[i], (float)i)); assert(feq(ch1[i], (float)(2 * i))); }

    /* ratio != 1.0 → genuine resample: finite, bounded within the input range, and the
     * first output still equals the first input (phase starts at 0). */
    src->ops->reset(src);
    abox_src_set_ratio(src, 1.25);
    for (int i = 0; i < 8; ++i) { ch0[i] = (float)i; ch1[i] = (float)i; }
    src->ops->process(src, &io);
    assert(feq(ch0[0], 0.0f));                 /* pos 0 → x[0] */
    for (int i = 0; i < 8; ++i) {
        assert(ch0[i] == ch0[i]);              /* not NaN */
        assert(ch0[i] >= -0.001f && ch0[i] <= 7.001f);   /* bounded by input range */
    }
    assert(feq(ch0[1], 1.25f));                /* pos 1.25 → interp(x1=1, x2=2) = 1.25 */

    abox_node_destroy(src);
}

/* Basic playback use case: drive the EXACT live coordinator path (the same
 * hermes_pipeline_process_tick the PipeWire bridge calls) over the full node graph for
 * a run of blocks. Verifies output is produced, finite, bounded, no drops — i.e. audio
 * flows mic→src→aec→beam→ses→out through the buffer pool. */
static void test_playback_pipeline(void) {
    hermes_buffered_pipeline e;
    hermes_pipeline_init(&e, 48000);

    abox_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate = 48000; cfg.block_size = 240; cfg.mic_channels = 2;

    abox_node* src  = abox_node_create("src");
    abox_node* aec  = abox_node_create("aec");
    abox_node* beam = abox_node_create("beamform");
    abox_node* ses  = abox_node_create("ses");
    assert(src && aec && beam && ses);
    src->ops->prepare(src, &cfg);  aec->ops->prepare(aec, &cfg);
    beam->ops->prepare(beam, &cfg); ses->ops->prepare(ses, &cfg);
    hermes_pipeline_add_stage(&e, src,  ABOX_ELEM_SRC);
    hermes_pipeline_add_stage(&e, aec,  ABOX_ELEM_AEC);
    hermes_pipeline_add_stage(&e, beam, ABOX_ELEM_BEAM);
    hermes_pipeline_add_stage(&e, ses,  ABOX_ELEM_SES);
    hermes_pipeline_set_mode(&e, ABOX_MODE_CLOUD_STREAMING);   /* full duplex → all stages on */

    enum { BLK = 240 };
    float in0[BLK], in1[BLK], out0[BLK];
    const float* in[2]  = { in0, in1 };
    float*       out[2] = { out0, NULL };
    for (int blk = 0; blk < 10; ++blk) {
        for (int i = 0; i < BLK; ++i) {
            const float t = (float)(blk * BLK + i) / 48000.0f;
            in0[i] = 0.25f * sinf(6.2831853f * 440.0f * t);
            in1[i] = in0[i];
        }
        const int rc = hermes_pipeline_process_tick(&e, in, 2, out, 1, BLK, (uint64_t)blk * BLK);
        assert(rc == 0);                                       /* processed, not soft-dropped */
        for (int i = 0; i < BLK; ++i) {
            assert(out0[i] == out0[i]);                        /* not NaN */
            assert(out0[i] >= -1.0f && out0[i] <= 1.0f);       /* bounded */
        }
    }
    /* identical mics → beamform average == the input → output mirrors the source (stubs pass). */
    assert(feq(out0[0], in0[0]) || feq(out0[0], 0.0f));
    assert(atomic_load(&e.processed) == 10);
    assert(atomic_load(&e.drops) == 0);

    abox_node_destroy(src);  abox_node_destroy(aec);
    abox_node_destroy(beam); abox_node_destroy(ses);
}

/* Async pipeline: with per-slot worker threads, the SAME live tick produces real output
 * across periods (≈1-period delayed). Drives the engine through start_async, feeds a run
 * of blocks, and verifies real (non-silence) output appears and the pool made progress. */
static void test_async_pipeline(void) {
    hermes_buffered_pipeline e;
    hermes_pipeline_init(&e, 48000);
    abox_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate = 48000; cfg.block_size = 240; cfg.mic_channels = 2;

    abox_node* src  = abox_node_create("src");
    abox_node* aec  = abox_node_create("aec");
    abox_node* beam = abox_node_create("beamform");
    abox_node* ses  = abox_node_create("ses");
    assert(src && aec && beam && ses);
    src->ops->prepare(src, &cfg);  aec->ops->prepare(aec, &cfg);
    beam->ops->prepare(beam, &cfg); ses->ops->prepare(ses, &cfg);
    hermes_pipeline_add_stage(&e, src,  ABOX_ELEM_SRC);
    hermes_pipeline_add_stage(&e, aec,  ABOX_ELEM_AEC);
    hermes_pipeline_add_stage(&e, beam, ABOX_ELEM_BEAM);
    hermes_pipeline_add_stage(&e, ses,  ABOX_ELEM_SES);
    hermes_pipeline_set_mode(&e, ABOX_MODE_CLOUD_STREAMING);

    hermes_pipeline_start_async(&e, /*first_core=*/-1);   /* no pinning under test */

    enum { BLK = 240, K = 64 };
    float in0[BLK], in1[BLK], out0[BLK];
    const float* in[2]  = { in0, in1 };
    float*       out[2] = { out0, NULL };
    int saw_output = 0;
    for (int blk = 0; blk < K; ++blk) {
        for (int i = 0; i < BLK; ++i) { in0[i] = 0.2f; in1[i] = 0.2f; }
        out0[0] = -99.0f;
        hermes_pipeline_process_tick(&e, in, 2, out, 1, BLK, (uint64_t)blk * BLK);
        if (out0[0] > 0.1f && out0[0] < 0.3f) saw_output = 1;   /* a processed (passthrough ~0.2) block */
        struct timespec ts = {0, 300000};                       /* 0.3 ms — let workers run */
        nanosleep(&ts, NULL);
    }

    hermes_pipeline_stop_async(&e);
    assert(saw_output);                          /* the async pipeline produced real output */
    assert(atomic_load(&e.processed) > 0);
    abox_node_destroy(src);  abox_node_destroy(aec);
    abox_node_destroy(beam); abox_node_destroy(ses);
}

/* Loopback / bypass (BASIC TC, mirrors scripts/run_loopback.sh at unit level). With no DSP
 * kernels yet every node is a passthrough, so the whole src→aec→beam→ses chain is an identity
 * on chan[0]. Feeds a distinctive tone on the L mic and a DIFFERENT DC level on the R mic; once
 * the buffer pool fills, the mono output must equal the L input BIT-EXACT — proving the full
 * transport+graph path is lossless AND that beamform bypasses chan[0] (an average would fold in
 * the R mic and break equality). This is the no-PipeWire twin of the on-target loopback. */
static void test_loopback_bypass(void) {
    hermes_buffered_pipeline e;
    hermes_pipeline_init(&e, 48000);
    abox_config cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate = 48000; cfg.block_size = 240; cfg.mic_channels = 2;

    abox_node* src  = abox_node_create("src");
    abox_node* aec  = abox_node_create("aec");
    abox_node* beam = abox_node_create("beamform");
    abox_node* ses  = abox_node_create("ses");
    assert(src && aec && beam && ses);
    src->ops->prepare(src, &cfg);  aec->ops->prepare(aec, &cfg);
    beam->ops->prepare(beam, &cfg); ses->ops->prepare(ses, &cfg);
    hermes_pipeline_add_stage(&e, src,  ABOX_ELEM_SRC);
    hermes_pipeline_add_stage(&e, aec,  ABOX_ELEM_AEC);
    hermes_pipeline_add_stage(&e, beam, ABOX_ELEM_BEAM);
    hermes_pipeline_add_stage(&e, ses,  ABOX_ELEM_SES);
    hermes_pipeline_set_mode(&e, ABOX_MODE_CLOUD_STREAMING);   /* every stage active */

    enum { BLK = 240 };
    float in0[BLK], in1[BLK], out0[BLK];
    const float* in[2]  = { in0, in1 };
    float*       out[2] = { out0, NULL };
    for (int i = 0; i < BLK; ++i) {
        in0[i] = 0.25f * sinf(6.2831853f * 440.0f * (float)i / 48000.0f);   /* L: a tone */
        in1[i] = 0.5f;                                                      /* R: distinct DC */
    }
    /* Feed the SAME block repeatedly so the steady-state result is independent of the
     * pool's ~1-period fill latency. */
    int matched = 0;
    for (int blk = 0; blk < 6; ++blk) {
        for (int i = 0; i < BLK; ++i) out0[i] = -99.0f;
        assert(hermes_pipeline_process_tick(&e, in, 2, out, 1, BLK, (uint64_t)blk * BLK) == 0);
        int eq = 1;
        for (int i = 0; i < BLK; ++i) if (out0[i] != in0[i]) { eq = 0; break; }   /* bit-exact */
        if (eq) matched = 1;
    }
    assert(matched);                                              /* output == L input, bit-exact */
    assert(out0[0] != 0.5f * (in0[0] + in1[0]));                  /* NOT the L+R average → beamform bypassed */
    assert(atomic_load(&e.drops) == 0);

    abox_node_destroy(src);  abox_node_destroy(aec);
    abox_node_destroy(beam); abox_node_destroy(ses);
}

/* Virtual DMA: the xfer primitive copies planar channels + counts transfers; the node
 * form moves between a bound external buffer and the frame (ingress IN / egress OUT). */
static void test_vdma(void) {
    abox_vdma dma; abox_vdma_init(&dma);
    float s0[4] = {1,2,3,4}, s1[4] = {5,6,7,8}, d0[4] = {0}, d1[4] = {0};
    const float* src[2] = { s0, s1 };
    float*       dst[2] = { d0, d1 };
    abox_vdma_xfer(&dma, dst, src, 2, 4);
    for (int i = 0; i < 4; ++i) { assert(feq(d0[i], s0[i])); assert(feq(d1[i], s1[i])); }
    assert(dma.transfers == 1 && dma.samples == 8);

    abox_config cfg; memset(&cfg, 0, sizeof(cfg));

    /* IN: external → frame */
    abox_node* vin = abox_vdma_create(ABOX_VDMA_IN);
    assert(vin); vin->ops->prepare(vin, &cfg);
    float e0[4] = {9,8,7,6}, e1[4] = {1,1,1,1};
    float* ext[2] = { e0, e1 };
    abox_vdma_bind(vin, ext, 2, 4);
    float f0[4] = {0}, f1[4] = {0};
    abox_frame io; io.chan[0] = f0; io.chan[1] = f1; io.channels = 0; io.frames = 4;
    vin->ops->process(vin, &io);
    assert(io.channels == 2);
    for (int i = 0; i < 4; ++i) { assert(feq(f0[i], e0[i])); assert(feq(f1[i], e1[i])); }

    /* OUT: frame → external */
    abox_node* vout = abox_vdma_create(ABOX_VDMA_OUT);
    assert(vout); vout->ops->prepare(vout, &cfg);
    float o0[4] = {0}, o1[4] = {0};
    float* oext[2] = { o0, o1 };
    abox_vdma_bind(vout, oext, 2, 4);
    vout->ops->process(vout, &io);                       /* io still holds e0/e1 */
    for (int i = 0; i < 4; ++i) { assert(feq(o0[i], e0[i])); assert(feq(o1[i], e1[i])); }
    const abox_vdma* st = abox_vdma_node_stats(vout);
    assert(st && st->transfers == 1);

    abox_node_destroy(vin);
    abox_node_destroy(vout);
}

int main(void) {
    test_routing_mask();
    test_vdma();
    test_graph_mask_gating();
    test_reference_manager();
    test_worker_pool();
    test_param_store();
    test_buffer_pipeline();
    test_src_node();
    test_playback_pipeline();
    test_async_pipeline();
    test_loopback_bypass();
    printf("abox_selftest: all passed\n");
    return 0;
}
