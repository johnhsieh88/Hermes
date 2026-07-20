/* abox_nodes.c — node factory (SDS §12). Each node lives in its own source file under
 * nodes/ (src_node.c, aec_node.c, beamform_node.c, dmx_node.c, capgate_node.c) and exposes a constructor
 * via node_common.h; this file just maps a type name to the right constructor. */
#include "audio_core/abox/abox_nodes.h"
#include "audio_core/abox/nodes/node_common.h"
#include <string.h>

abox_node* abox_node_create(const char* type) {
    if (!type) return NULL;
    if (strcmp(type, "src")      == 0) return abox_src_create();
    if (strcmp(type, "aec")      == 0) return abox_aec_create();
    if (strcmp(type, "beamform") == 0) return abox_beamform_create();
    if (strcmp(type, "dmx")      == 0) return abox_dmx_create();
    if (strcmp(type, "capgate")  == 0) return abox_capgate_create();
    return NULL;
}

void abox_node_destroy(abox_node* n) {
    if (n && n->ops && n->ops->destroy) n->ops->destroy(n);
}
