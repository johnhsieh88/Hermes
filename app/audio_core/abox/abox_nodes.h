/* abox_nodes.h — concrete DSP nodes (SDS §4) as C vtable instances. Each node owns a
 * private state struct; construction mallocs once at init (NOT on the hot path) and
 * destroy() frees it. The AEC node optionally pulls its time-aligned far-end from a
 * reference_manager (§4.3.2). */
#ifndef HERMES_ABOX_NODES_H
#define HERMES_ABOX_NODES_H

#include "audio_core/abox/abox_node.h"
#include "audio_core/abox/reference_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Factory by type name ("src" | "aec" | "beamform" | "ses"). NULL on unknown / OOM. */
abox_node* abox_node_create(const char* type);
void       abox_node_destroy(abox_node* n);

/* AEC-specific wiring: the source of the aligned post-fader reference (§4.3.2). */
void abox_aec_set_ref(abox_node* aec, abox_ref_manager* ref);

/* SRC-specific: set the ASRC drift ratio (input samples per output, ≈1.0). The §5 PI
 * loop calls this each control tick; 1.0 is an exact identity. */
void abox_src_set_ratio(abox_node* src, double ratio);

#ifdef __cplusplus
}
#endif
#endif /* HERMES_ABOX_NODES_H */
