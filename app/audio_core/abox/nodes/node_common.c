/* node_common.c — node allocator + default vtable slots shared by every node file. */
#include "audio_core/abox/nodes/node_common.h"
#include <stdlib.h>

abox_node* abox_node_alloc(const abox_node_ops* ops, size_t state_size, int in_ch, int out_ch) {
    abox_node* n = (abox_node*)calloc(1, sizeof(abox_node));
    if (!n) return NULL;
    n->state = state_size ? calloc(1, state_size) : NULL;
    if (state_size && !n->state) { free(n); return NULL; }
    n->ops   = ops;
    n->in_ch = in_ch;
    n->out_ch = out_ch;
    return n;
}

void abox_node_default_prepare(abox_node* n, const abox_config* cfg) { (void)n; (void)cfg; }
void abox_node_default_configure(abox_node* n, uint32_t id)          { (void)n; (void)id; }
void abox_node_default_reset(abox_node* n)                           { (void)n; }
void abox_node_default_destroy(abox_node* n) { if (n) { free(n->state); free(n); } }
