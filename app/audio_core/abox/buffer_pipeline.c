/* buffer_pipeline.c — Core-Proportional Buffer Pool coordinator (SDD §3.1). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* CPU_SET / pthread_setaffinity_np */
#endif
#include "audio_core/abox/buffer_pipeline.h"
#include <stddef.h>
#include <string.h>

#if defined(__linux__)
#include <sched.h>
#endif

/* Slot lifecycle for the async path. */
enum { SLOT_FREE = 0, SLOT_READY = 1, SLOT_DONE = 2 };

static inline void bp_cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

static void bp_pin(int core_id, int prio) {
#if defined(__linux__)
    if (core_id >= 0) {
        cpu_set_t set; CPU_ZERO(&set); CPU_SET(core_id, &set);
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    }
    struct sched_param sp; sp.sched_priority = prio;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);   /* best-effort (needs privilege) */
#else
    (void)core_id; (void)prio;
#endif
}

void hermes_pipeline_init(hermes_buffered_pipeline* e, int sample_rate) {
    memset(e, 0, sizeof(*e));
    abox_graph_init(&e->graph);
    e->sample_rate   = sample_rate;
    e->next_core_idx = 0;
    e->pool          = NULL;
    atomic_store_explicit(&e->mode, ABOX_MODE_KEYWORD_LISTENING, memory_order_relaxed);
    for (int i = 0; i < HERMES_NUM_WORKER_CORES; ++i) {
        atomic_store_explicit(&e->core_in_progress[i], 0, memory_order_relaxed);
        for (int c = 0; c < ABOX_MAX_CHANNELS; ++c)
            e->worker_buffers[i].chan[c] = e->slot_mem[i][c];   /* bind slot to its own memory */
    }
    atomic_store_explicit(&e->drops, 0, memory_order_relaxed);
    atomic_store_explicit(&e->processed, 0, memory_order_relaxed);
}

int hermes_pipeline_add_stage(hermes_buffered_pipeline* e, abox_node* node, abox_elem elem) {
    return abox_graph_add(&e->graph, node, elem);
}

void hermes_pipeline_set_pool(hermes_buffered_pipeline* e, abox_worker_pool* pool) { e->pool = pool; }

void hermes_pipeline_set_mode(hermes_buffered_pipeline* e, abox_mode mode) {
    atomic_store_explicit(&e->mode, (int)mode, memory_order_release);
}

/* Job wrapper: run the whole mask-gated cascade on a borrowed A76 core (§10.7 fan-out). */
typedef struct { hermes_buffered_pipeline* e; abox_frame* io; abox_mode mode; } cascade_arg;
static void cascade_job(void* ctx, int slice, int frames) {
    (void)slice; (void)frames;
    cascade_arg* a = (cascade_arg*)ctx;
    abox_graph_tick(&a->e->graph, a->io, a->mode);
}

/* ── Async path: one worker thread per slot; real cross-period pipeline ── */
typedef struct { hermes_buffered_pipeline* e; int idx; } bp_slot_arg;
static bp_slot_arg g_bp_args[HERMES_NUM_WORKER_CORES];   /* single live engine */

static void* bp_slot_worker(void* arg) {
    bp_slot_arg* a = (bp_slot_arg*)arg;
    hermes_buffered_pipeline* e = a->e;
    const int i = a->idx;
    bp_pin(e->first_core >= 0 ? e->first_core + i : -1, /*prio=*/88);
    while (atomic_load_explicit(&e->async_run, memory_order_relaxed)) {
        if (atomic_load_explicit(&e->slot_state[i], memory_order_acquire) == SLOT_READY) {
            abox_graph_tick(&e->graph, &e->worker_buffers[i], e->slot_mode[i]);   /* cascade on THIS core */
            atomic_store_explicit(&e->slot_state[i], SLOT_DONE, memory_order_release);
        } else {
            for (int s = 0; s < 256; ++s) bp_cpu_relax();
#if defined(__linux__)
            sched_yield();
#endif
        }
    }
    return NULL;
}

void hermes_pipeline_start_async(hermes_buffered_pipeline* e, int first_core) {
    if (e->async_active) return;
    e->first_core = first_core;
    e->in_idx = 0;
    e->out_idx = 0;
    for (int i = 0; i < HERMES_NUM_WORKER_CORES; ++i)
        atomic_store_explicit(&e->slot_state[i], SLOT_FREE, memory_order_relaxed);
    atomic_store_explicit(&e->async_run, 1, memory_order_relaxed);
    e->async_active = 1;
    for (int i = 0; i < HERMES_NUM_WORKER_CORES; ++i) {
        g_bp_args[i].e = e;
        g_bp_args[i].idx = i;
        pthread_create(&e->worker[i], NULL, bp_slot_worker, &g_bp_args[i]);
    }
}

void hermes_pipeline_stop_async(hermes_buffered_pipeline* e) {
    if (!e->async_active) return;
    atomic_store_explicit(&e->async_run, 0, memory_order_relaxed);
    for (int i = 0; i < HERMES_NUM_WORKER_CORES; ++i)
        pthread_join(e->worker[i], NULL);
    e->async_active = 0;
}

/* Non-blocking tick: COLLECT a finished slot → out (≈1 period delayed; Soft-Mute on
 * underflow), then INGEST the new period into a free slot for a worker (overflow → drop). */
static int bp_tick_async(hermes_buffered_pipeline* e,
                         const float* const* in_chan, int in_channels,
                         float* const* out_chan, int out_channels,
                         int frames, uint64_t sample_pos) {
    if (frames > ABOX_MAX_BLOCK) frames = ABOX_MAX_BLOCK;
    if (in_channels  > ABOX_MAX_CHANNELS) in_channels  = ABOX_MAX_CHANNELS;
    if (out_channels > ABOX_MAX_CHANNELS) out_channels = ABOX_MAX_CHANNELS;

    /* 1) COLLECT — write at most out_channels ports (the produced channels, else silence). */
    const uint32_t o = e->out_idx;
    if (atomic_load_explicit(&e->slot_state[o], memory_order_acquire) == SLOT_DONE) {
        abox_frame* s = &e->worker_buffers[o];
        for (int c = 0; c < out_channels; ++c)
            if (out_chan && out_chan[c]) {
                if (c < s->channels && s->chan[c]) memcpy(out_chan[c], s->chan[c], sizeof(float) * (size_t)frames);
                else                               memset(out_chan[c], 0, sizeof(float) * (size_t)frames);
            }
        atomic_store_explicit(&e->slot_state[o], SLOT_FREE, memory_order_release);
        e->out_idx = (o + 1) % HERMES_NUM_WORKER_CORES;
        atomic_fetch_add_explicit(&e->processed, 1, memory_order_relaxed);
    } else {
        for (int c = 0; c < out_channels; ++c)                        /* underflow → Soft-Mute (§6.1) */
            if (out_chan && out_chan[c]) memset(out_chan[c], 0, sizeof(float) * (size_t)frames);
    }

    /* 2) INGEST */
    const uint32_t j = e->in_idx;
    if (atomic_load_explicit(&e->slot_state[j], memory_order_acquire) == SLOT_FREE) {
        abox_frame* d = &e->worker_buffers[j];
        for (int c = 0; c < in_channels; ++c)
            if (in_chan && in_chan[c]) memcpy(d->chan[c], in_chan[c], sizeof(float) * (size_t)frames);
        d->channels = in_channels;
        d->frames   = frames;
        d->sample_pos = sample_pos;
        d->rate     = (uint32_t)e->sample_rate;
        e->slot_mode[j] = (abox_mode)atomic_load_explicit(&e->mode, memory_order_acquire);
        d->mode = e->slot_mode[j];
        atomic_store_explicit(&e->slot_state[j], SLOT_READY, memory_order_release);   /* wake worker[j] */
        e->in_idx = (j + 1) % HERMES_NUM_WORKER_CORES;
        return 0;
    }
    atomic_fetch_add_explicit(&e->drops, 1, memory_order_relaxed);   /* all slots busy → overflow drop */
    return 1;
}

int hermes_pipeline_process_tick(hermes_buffered_pipeline* e,
                                 const float* const* in_chan, int in_channels,
                                 float* const* out_chan, int out_channels,
                                 int frames, uint64_t sample_pos) {
    if (e->async_active)
        return bp_tick_async(e, in_chan, in_channels, out_chan, out_channels, frames, sample_pos);

    const uint32_t target = e->next_core_idx;
    if (frames > ABOX_MAX_BLOCK) frames = ABOX_MAX_BLOCK;
    if (in_channels  > ABOX_MAX_CHANNELS) in_channels  = ABOX_MAX_CHANNELS;
    if (out_channels > ABOX_MAX_CHANNELS) out_channels = ABOX_MAX_CHANNELS;

    /* §3.1 step 3 — LOCK-FREE FIREWALL: if this core's prior period is still in flight,
     * soft-drop now. The driver ping-pong keeps progressing, so no ALSA Xrun occurs. */
    if (atomic_load_explicit(&e->core_in_progress[target], memory_order_acquire)) {
        atomic_fetch_add_explicit(&e->drops, 1, memory_order_relaxed);
        return 1;
    }

    /* §3.1 step 4 — engage the busy indicator for this slot. */
    atomic_store_explicit(&e->core_in_progress[target], 1, memory_order_release);

    /* §3.1 step 5 — copy the read-only driver input into this slot's provisioned memory,
     * decoupling processing from the driver buffer (so the slot can be held across periods). */
    abox_frame* io = &e->worker_buffers[target];
    for (int c = 0; c < in_channels; ++c)
        if (in_chan && in_chan[c]) memcpy(io->chan[c], in_chan[c], sizeof(float) * (size_t)frames);
    io->channels   = in_channels;
    io->frames     = frames;
    io->sample_pos = sample_pos;
    io->rate       = (uint32_t)e->sample_rate;
    const abox_mode mode = (abox_mode)atomic_load_explicit(&e->mode, memory_order_acquire);
    io->mode = mode;

    /* Stagger to the next slot for the subsequent period (proportional double-buffer). */
    e->next_core_idx = (target + 1) % HERMES_NUM_WORKER_CORES;

    /* §3.1 step 6 — mask-gated VTABLE cascade (ABOX_XFER_INPLACE). With a standing pool,
     * dispatch the cascade to an A76 worker; otherwise run inline on the caller. */
    if (e->pool) {
        cascade_arg a = { e, io, mode };
        abox_cmd cmd = { cascade_job, &a, (int)target, frames };
        abox_pool_run(e->pool, &cmd, 1, 0, NULL);
    } else {
        abox_graph_tick(&e->graph, io, mode);
    }

    /* Egress: slot result → the PipeWire playback buffer (≤ out_channels ports). */
    for (int c = 0; c < out_channels; ++c)
        if (out_chan && out_chan[c]) {
            if (c < io->channels && io->chan[c]) memcpy(out_chan[c], io->chan[c], sizeof(float) * (size_t)frames);
            else                                 memset(out_chan[c], 0, sizeof(float) * (size_t)frames);
        }

    /* §3.1 step 7 — release the slot. */
    atomic_store_explicit(&e->core_in_progress[target], 0, memory_order_release);
    atomic_fetch_add_explicit(&e->processed, 1, memory_order_relaxed);
    return 0;
}
