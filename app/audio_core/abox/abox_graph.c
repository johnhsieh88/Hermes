/* abox_graph.c — in-process graph execution (SDD §2 / SDS §10.9 tier-1 sequential).
 * A static stage array is walked in signal-chain order each block; the tick loop
 * evaluates active_pipeline_mask to include or skip each stage by use-case, in place,
 * with no re-linking and no runtime allocation. */
#include "audio_core/abox/abox_node.h"
#include "hermes/common/Log.h"
#include <stdlib.h>
#include <time.h>

/* Dev-only per-node trace — HERMES_ABOX_TRACE=N logs every Nth block (1 = every block,
 * 200 = 1 Hz heartbeat @ 5 ms). OFF unless the env var is set: fprintf on the RT path is
 * blocking I/O and violates NFR-8, so this exists for bring-up/diagnosis ONLY — never in
 * latency measurements or production. Enter log before process(), exit log after it and
 * before the next stage runs. */
static int      g_trace_every = -1;              /* -1 = env not read yet */
static uint64_t g_trace_block = 0;

/* Monotonic wall clock in µs (vDSO read — non-blocking, cheap; the fprintf is the
 * NFR-8 concern, not this). Printed as s:µs; per-node dt on the exit line. */
static uint64_t trace_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

void abox_graph_init(abox_graph* g) {
    g->count = 0;
    if (g_trace_every < 0) {
        const char* t = getenv("HERMES_ABOX_TRACE");
        g_trace_every = t ? atoi(t) : 0;
        if (g_trace_every > 0)
            HM_LOG_INFO("abox node trace ON — every %d block(s); DEV ONLY (breaks NFR-8)",
                        g_trace_every);
    }
}

int abox_graph_add(abox_graph* g, abox_node* node, abox_elem elem) {
    if (!g || !node || g->count >= ABOX_MAX_STAGES) return -1;
    g->stages[g->count].node = node;
    g->stages[g->count].elem = elem;
    return g->count++;
}

int abox_graph_tick(abox_graph* g, abox_frame* io, abox_mode mode) {
    const uint32_t mask = abox_active_mask(mode);
    io->mode = mode;
    const int trace = g_trace_every > 0 &&
                      (g_trace_block++ % (uint64_t)g_trace_every) == 0;
    int ran = 0;
    for (int i = 0; i < g->count; ++i) {
        abox_stage* s = &g->stages[i];
        if (s->elem == ABOX_ELEM_STRUCTURAL ||      /* structural stages run in EVERY mode */
            (mask & abox_elem_bit(s->elem))) {      /* INCLUDE: run the block in place */
            uint64_t t0 = 0;
            if (trace) {
                t0 = trace_now_us();                /* monotonic, for the dt only — the log
                                                       prefix carries the wall timestamp */
                HM_LOG_DEBUG(">> %-9s ch=%d fr=%d pos=%llu mode=%d",
                             s->node->name ? s->node->name : "(anon)",
                             io->channels, io->frames,
                             (unsigned long long)io->sample_pos, (int)mode);
            }
            s->node->ops->process(s->node, io);
            if (trace) {                            /* exit log BEFORE the next stage runs */
                HM_LOG_DEBUG("<< %-9s ch=%d dt=%lluus",
                             s->node->name ? s->node->name : "(anon)", io->channels,
                             (unsigned long long)(trace_now_us() - t0));
            }
            ++ran;
        }
        /* else SKIP: zero-copy passthrough — io already carries the data, bounds unchanged. */
    }
    return ran;
}
