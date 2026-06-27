/* node_common.h — shared scaffolding for the per-file DSP nodes (SDS §4). Holds the
 * node allocator and the no-op default vtable slots reused by every node, plus each
 * node type's constructor (the factory in abox_nodes.c dispatches to these). One node
 * per source file (src_node.c, aec_node.c, beamform_node.c, ses_node.c). */
#ifndef HERMES_ABOX_NODE_COMMON_H
#define HERMES_ABOX_NODE_COMMON_H

#include <stddef.h>
#include "audio_core/abox/abox_node.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate a node + zeroed private state (malloc at init only, never on the hot path). */
abox_node* abox_node_alloc(const abox_node_ops* ops, size_t state_size, int in_ch, int out_ch);

/* No-op default vtable slots — a node wires these for the ops it does not need. */
void abox_node_default_prepare(abox_node* n, const abox_config* cfg);
void abox_node_default_configure(abox_node* n, uint32_t param_set_id);
void abox_node_default_reset(abox_node* n);
void abox_node_default_destroy(abox_node* n);

/* Per-type constructors — one per node source file. */
abox_node* abox_src_create(void);
abox_node* abox_aec_create(void);
abox_node* abox_beamform_create(void);
abox_node* abox_ses_create(void);

#ifdef __cplusplus
}
#endif
#endif /* HERMES_ABOX_NODE_COMMON_H */
