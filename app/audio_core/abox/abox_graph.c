/* abox_graph.c — in-process graph execution (SDD §2 / SDS §10.9 tier-1 sequential).
 * A static stage array is walked in signal-chain order each block; the tick loop
 * evaluates active_pipeline_mask to include or skip each stage by use-case, in place,
 * with no re-linking and no runtime allocation. */
#include "audio_core/abox/abox_node.h"

void abox_graph_init(abox_graph* g) {
    g->count = 0;
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
    int ran = 0;
    for (int i = 0; i < g->count; ++i) {
        abox_stage* s = &g->stages[i];
        if (mask & abox_elem_bit(s->elem)) {        /* INCLUDE: run the block in place */
            s->node->ops->process(s->node, io);
            ++ran;
        }
        /* else SKIP: zero-copy passthrough — io already carries the data, bounds unchanged. */
    }
    return ran;
}
