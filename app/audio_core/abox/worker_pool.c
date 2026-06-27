#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* CPU_ZERO/CPU_SET, pthread_setaffinity_np (glibc) */
#endif
#include "audio_core/abox/worker_pool.h"
#include <stddef.h>

#if defined(__linux__)
#include <sched.h>
#endif

/* CPU relax hint for the spin sections (§11.2) — sub-µs wake while busy. */
static inline void abox_cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

/* Best-effort pin to a core with SCHED_FIFO (real target = PREEMPT_RT; dev host = no-op). */
static void abox_pin(int core_id, int prio) {
#if defined(__linux__)
    if (core_id >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(core_id, &set);
        pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    }
    struct sched_param sp;
    sp.sched_priority = prio;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
#else
    (void)core_id; (void)prio;
#endif
}

#define ABOX_POOL_SPIN_LIMIT 1500   /* ~tens of µs of spin before parking (§11.2) */

/* Self-claim drain — run by both the dedicated workers AND the coordinator. Mutual
 * exclusion comes from the uniqueness of the fetch_add index, not from a lock. */
static void abox_pool_drain(abox_worker_pool* p) {
    for (;;) {
        const int idx = atomic_fetch_add_explicit(&p->next_job, 1, memory_order_relaxed);
        if (idx >= p->job_count) return;
        abox_cmd* j = &p->jobs[idx];
        if (j->fn && !atomic_load_explicit(&p->force_abort, memory_order_relaxed))
            j->fn(j->ctx, j->slice, j->frames);
        atomic_fetch_sub_explicit(&p->done_ctr, 1, memory_order_release);
    }
}

typedef struct { abox_worker_pool* pool; int idx; } abox_worker_arg;

/* Worker hot loop: HYBRID spin/park on the generation counter. (Portable build spins
 * then yields via sched_yield; swap for futex_wait on the PREEMPT_RT target for 0% CPU.) */
static void* abox_worker_main(void* arg_) {
    abox_worker_arg* a = (abox_worker_arg*)arg_;
    abox_worker_pool* p = a->pool;
    const int worker_idx = a->idx;
    abox_pin(p->first_core >= 0 ? p->first_core + worker_idx : -1, /*prio=*/89);
    (void)worker_idx;

    uint32_t seen = atomic_load_explicit(&p->generation, memory_order_acquire);
    while (atomic_load_explicit(&p->running, memory_order_relaxed)) {
        uint32_t gen = atomic_load_explicit(&p->generation, memory_order_acquire);
        if (gen == seen) {
            int armed = 0;
            for (int s = 0; s < ABOX_POOL_SPIN_LIMIT; ++s) {
                abox_cpu_relax();
                gen = atomic_load_explicit(&p->generation, memory_order_acquire);
                if (gen != seen) { armed = 1; break; }
            }
            if (!armed) {
                atomic_fetch_add_explicit(&p->parked, 1, memory_order_relaxed);
                while (atomic_load_explicit(&p->running, memory_order_relaxed) &&
                       atomic_load_explicit(&p->generation, memory_order_acquire) == seen) {
#if defined(__linux__)
                    sched_yield();
#endif
                }
                atomic_fetch_sub_explicit(&p->parked, 1, memory_order_relaxed);
                continue;
            }
        }
        seen = gen;
        abox_pool_drain(p);
    }
    return NULL;
}

/* Per-worker args kept alive for the pool lifetime (one slot per possible worker). */
static abox_worker_arg g_worker_args[ABOX_MAX_WORKERS];

void abox_pool_start(abox_worker_pool* p, int n_cores, int first_core) {
    if (atomic_load_explicit(&p->running, memory_order_relaxed)) return;
    p->jobs = NULL;
    p->job_count = 0;
    atomic_store_explicit(&p->next_job, 0, memory_order_relaxed);
    atomic_store_explicit(&p->done_ctr, 0, memory_order_relaxed);
    atomic_store_explicit(&p->generation, 0, memory_order_relaxed);
    atomic_store_explicit(&p->force_abort, 0, memory_order_relaxed);
    atomic_store_explicit(&p->parked, 0, memory_order_relaxed);
    atomic_store_explicit(&p->running, 1, memory_order_relaxed);
    p->first_core = first_core;

    int nw = (n_cores > 1) ? n_cores - 1 : 0;          /* coordinator is the N-th context */
    if (nw > ABOX_MAX_WORKERS) nw = ABOX_MAX_WORKERS;
    p->n_workers = nw;
    for (int i = 0; i < nw; ++i) {
        g_worker_args[i].pool = p;
        g_worker_args[i].idx  = i;
        pthread_create(&p->worker[i], NULL, abox_worker_main, &g_worker_args[i]);
    }
}

void abox_pool_stop(abox_worker_pool* p) {
    if (!atomic_exchange_explicit(&p->running, 0, memory_order_relaxed)) return;
    atomic_fetch_add_explicit(&p->generation, 1, memory_order_release);   /* wake parked workers */
    for (int i = 0; i < p->n_workers; ++i)
        pthread_join(p->worker[i], NULL);
    p->n_workers = 0;
}

int abox_pool_run(abox_worker_pool* p, abox_cmd* jobs, int njobs,
                  uint64_t deadline_ns, uint64_t (*now_ns)(void)) {
    if (njobs <= 0) return 1;
    atomic_store_explicit(&p->force_abort, 0, memory_order_relaxed);
    p->jobs = jobs;
    p->job_count = njobs;
    atomic_store_explicit(&p->next_job, 0, memory_order_relaxed);
    atomic_store_explicit(&p->done_ctr, njobs, memory_order_relaxed);
    atomic_fetch_add_explicit(&p->generation, 1, memory_order_release);   /* publish + wake */

    abox_pool_drain(p);                                                   /* coordinator joins drain */

    while (atomic_load_explicit(&p->done_ctr, memory_order_acquire) > 0) {
        if (deadline_ns && now_ns && now_ns() > deadline_ns) {
            atomic_store_explicit(&p->force_abort, 1, memory_order_relaxed);
            return 0;                                                     /* overrun → Soft-Mute */
        }
        abox_cpu_relax();
    }
    return 1;
}
