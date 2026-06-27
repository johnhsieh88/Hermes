/* abox_event.h — RT→world event path (SDS §13/§14.8). The RT data-loop must never call
 * MsgBus (no syscalls/malloc on the data-loop, §11.4), so a DSP node pushes a small POD
 * event into this lock-free SPSC ring; a non-RT forwarder drains it and SendMsg's it.
 * Ids are kept Catalog-agnostic — the forwarder maps them to §14.5 Catalog ids. */
#ifndef HERMES_ABOX_EVENT_H
#define HERMES_ABOX_EVENT_H

#include <stdint.h>
#include "audio_core/abox/abox_atomic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ABOX_EVT_NONE = 0,
    ABOX_EVT_BARGE_IN     /* sustained local speech during playback (§8) */
} abox_event_id;

typedef struct {
    uint16_t id;          /* abox_event_id */
    uint64_t sample_pos;  /* PipeWire timeline at detection */
} abox_event;

#define ABOX_EVENT_RING_CAP 64u   /* power of two */

/* SPSC lock-free ring: ONE RT producer (push), ONE non-RT consumer (pop). */
typedef struct {
    abox_event            slot[ABOX_EVENT_RING_CAP];
    ABOX_ATOMIC(uint32_t) head;   /* producer cursor (monotonic) */
    ABOX_ATOMIC(uint32_t) tail;   /* consumer cursor (monotonic) */
} abox_event_ring;

void abox_event_ring_init(abox_event_ring* r);
int  abox_event_push(abox_event_ring* r, uint16_t id, uint64_t sample_pos);  /* RT:  0 ok, -1 full */
int  abox_event_pop (abox_event_ring* r, abox_event* out);                   /* fwd: 0 ok, -1 empty */

#ifdef __cplusplus
}
#endif
#endif /* HERMES_ABOX_EVENT_H */
