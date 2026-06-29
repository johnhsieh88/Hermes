/* abox_routing.c — the declarative routing matrix (SDS §4.0.1 / §10.9). Per mode, the
 * activation gain (0..1) for each controllable element; the active_pipeline_mask is the
 * set of elements with gain > 0, derived here and read once per block on the RT path. */
#include "audio_core/abox/abox_node.h"

/* rows indexed by abox_mode; columns by abox_elem.
 *                              SRC  AEC  REF  BEAM SES  CAP  TTS */
static const float kGain[4][ABOX_ELEM_COUNT] = {
    /* KEYWORD_LISTENING (0) */ {0,   0,   0,   0,   0,   0,   0},  /* idle/wake — all bypassed */
    /* BARGE_IN_MUTING   (1) */ {1,   1,   1,   1,   1,   1,   0},  /* TTS ducked → 0, keep capture */
    /* CONVERSATION   (2) */ {1,   1,   1,   1,   1,   1,   1},  /* full duplex conversation */
    /* SYSTEM_RESET      (3) */ {0,   0,   0,   0,   0,   0,   0},  /* safe/muted during reset */
};

float abox_route_gain(abox_mode m, abox_elem e) {
    return kGain[(int)m & 3][(int)e];
}

uint32_t abox_active_mask(abox_mode m) {
    const int mi = (int)m & 3;
    uint32_t bits = 0;
    for (int e = 0; e < ABOX_ELEM_COUNT; ++e)
        if (kGain[mi][e] > 0.0f) bits |= abox_elem_bit((abox_elem)e);
    return bits;
}
