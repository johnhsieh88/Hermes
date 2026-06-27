/* buffer_pipeline.h — the Core-Proportional Buffer Pool & in-process graph coordinator
 * (SDD "Multi-Core Proportional Buffer Pool", §2.2/§3.1). One frame slot per reserved A76
 * worker core, each guarded by a lock-free atomic busy indicator (the firewall). On each
 * period the coordinator picks the next core's slot, copies the (read-only) driver input
 * into that slot's own provisioned memory, runs the mask-gated cascade in place, and
 * copies the result to the output buffer. If the target core is still processing a prior
 * period (overrun), the period is soft-dropped to protect the ALSA cadence — no driver
 * Xrun, recovery stays in userspace. The per-core slot memory decouples processing from
 * the driver buffer (so a slow core can hold its slot while the next period rotates on). */
#ifndef HERMES_ABOX_BUFFER_PIPELINE_H
#define HERMES_ABOX_BUFFER_PIPELINE_H

#include "audio_core/abox/abox_node.h"
#include "audio_core/abox/worker_pool.h"
#include "audio_core/abox/abox_atomic.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HERMES_NUM_WORKER_CORES
#define HERMES_NUM_WORKER_CORES 2   /* reserved A76 performance cores (RK3588 cpu6, cpu7) */
#endif

typedef struct {
    abox_graph        graph;        /* the mask-gated DSP route (abox_graph_tick) */
    abox_worker_pool* pool;         /* optional A76 standing pool; NULL = run inline on caller */
    ABOX_ATOMIC(int)  mode;         /* engine mode — control plane → RT (atomic swap, §4.2) */
    int               sample_rate;

    /* Core-allocated frame slots, provisioned at init — the proportional buffer pool.
     * Each slot owns its sample memory (slot_mem); worker_buffers[i].chan[c] points into it. */
    abox_frame  worker_buffers[HERMES_NUM_WORKER_CORES];
    float       slot_mem[HERMES_NUM_WORKER_CORES][ABOX_MAX_CHANNELS][ABOX_MAX_BLOCK];
    uint32_t         next_core_idx;
    ABOX_ATOMIC(int) core_in_progress[HERMES_NUM_WORKER_CORES];   /* lock-free busy indicators (sync path) */

    ABOX_ATOMIC(unsigned long) drops;        /* soft-drops on overrun (firewall fired) */
    ABOX_ATOMIC(unsigned long) processed;    /* blocks processed to completion */

    /* ── Async mode (one worker thread per slot; real cross-period pipeline) ── */
    int              async_active;                               /* set once at start_async (RT reads) */
    int              first_core;                                 /* worker[i] pinned to first_core+i (<0 = no pin) */
    uint32_t         in_idx;                                     /* RT producer: slot to FILL */
    uint32_t         out_idx;                                    /* RT consumer: slot to COLLECT (trails in_idx) */
    ABOX_ATOMIC(int) async_run;                                  /* workers run while set */
    ABOX_ATOMIC(int) slot_state[HERMES_NUM_WORKER_CORES];        /* FREE→READY→DONE per slot */
    abox_mode        slot_mode[HERMES_NUM_WORKER_CORES];         /* mode captured at ingest */
    pthread_t        worker[HERMES_NUM_WORKER_CORES];
} hermes_buffered_pipeline;

void hermes_pipeline_init(hermes_buffered_pipeline* e, int sample_rate);
int  hermes_pipeline_add_stage(hermes_buffered_pipeline* e, abox_node* node, abox_elem elem);
void hermes_pipeline_set_pool(hermes_buffered_pipeline* e, abox_worker_pool* pool);
void hermes_pipeline_set_mode(hermes_buffered_pipeline* e, abox_mode mode);

/* The coordinator hook (SDD §3.1). in_chan[0..in_channels) are the PipeWire capture-port
 * buffers (read-only); out_chan[0..out_channels) are the playback-port buffers. Input/output
 * channel counts differ (e.g. 2 mic in, 1 mono out after beamforming), so they are passed
 * separately — the egress writes at most out_channels ports. Returns 0 = processed,
 * 1 = soft-dropped (overrun). */
int hermes_pipeline_process_tick(hermes_buffered_pipeline* e,
                                 const float* const* in_chan, int in_channels,
                                 float* const* out_chan, int out_channels,
                                 int frames, uint64_t sample_pos);

/* Engage the ASYNC pipeline: spawn one worker thread per slot (pinned to first_core+i;
 * pass first_core < 0 to skip pinning). process_tick then becomes non-blocking — it
 * COLLECTs a finished slot to the output (≈1-period delayed) and INGESTs the new period
 * into a free slot for a worker, so a slow block on one core no longer stalls the loop
 * (a slow block degrades to one Soft-Mute period; an all-slots-busy state soft-drops).
 * Call once after the graph is built, before the data-loop starts. */
void hermes_pipeline_start_async(hermes_buffered_pipeline* e, int first_core);
void hermes_pipeline_stop_async(hermes_buffered_pipeline* e);

#ifdef __cplusplus
}
#endif
#endif /* HERMES_ABOX_BUFFER_PIPELINE_H */
