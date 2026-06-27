/* worker_pool.h — the §11.2 multi-core seam: a STANDING pool of one pinned thread per
 * big DSP core (RK3588 A76 = cpu4..7). The coordinator publishes a job array and bumps a
 * generation counter; every context (workers + coordinator) self-claims the next job via
 * fetch_add on a shared cursor. Join is a localized spin-wait on done_ctr, bounded by the
 * per-block deadline watchdog (§6.1). No mutex, no malloc, no kernel wait on the hot path. */
#ifndef HERMES_ABOX_WORKER_POOL_H
#define HERMES_ABOX_WORKER_POOL_H

#include <stdint.h>
#include <pthread.h>
#include "audio_core/abox/abox_atomic.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ABOX_MAX_WORKERS 4

/* One unit of parallel DSP work (the §11.2 OptimizedAudioJob). Run-to-completion. */
typedef struct {
    void (*fn)(void* ctx, int slice, int frames);   /* the kernel to run */
    void* ctx;                                       /* node state / buffers */
    int   slice;                                     /* e.g. channel × sub-band index */
    int   frames;
} abox_cmd;

typedef struct abox_worker_pool {
    abox_cmd*        jobs;
    int              job_count;
    ABOX_ATOMIC(int)      next_job;    /* self-claim cursor (fetch_add) */
    ABOX_ATOMIC(int)      done_ctr;    /* the join barrier (jobsRemaining) */
    ABOX_ATOMIC(uint32_t) generation;  /* bumped per dispatch → wakes parked workers */
    ABOX_ATOMIC(int)      running;
    ABOX_ATOMIC(int)      force_abort;
    ABOX_ATOMIC(int)      parked;      /* workers currently parked (cheap idle count) */
    int              n_workers;
    int              first_core;
    pthread_t        worker[ABOX_MAX_WORKERS];
} abox_worker_pool;

/* Bring up n_cores contexts: (n_cores-1) pinned worker threads + the caller as coordinator.
 * first_core is the core id the first worker pins to (RK3588: 5 → cpu5..7, coord on cpu4). */
void abox_pool_start(abox_worker_pool* p, int n_cores, int first_core);
void abox_pool_stop(abox_worker_pool* p);

/* Dispatch njobs and run them across the pool. The coordinator joins the drain then
 * spin-waits on done_ctr; if now_ns/deadline_ns are given and the deadline trips, sets
 * force_abort and returns 0 (caller emits Soft-Mute). Returns 1 on full completion. */
int abox_pool_run(abox_worker_pool* p, abox_cmd* jobs, int njobs,
                  uint64_t deadline_ns, uint64_t (*now_ns)(void));

#ifdef __cplusplus
}
#endif
#endif /* HERMES_ABOX_WORKER_POOL_H */
