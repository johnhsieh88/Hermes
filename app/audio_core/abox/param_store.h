/* param_store.h — lock-free double-buffered parameter handoff (SDD §4.2). A non-RT
 * control thread fills the inactive slot fully, then publishes it with a release store;
 * the RT process() reads the live slot with an acquire load on its next block — no torn
 * read, no mutex. Used for the 4-mic↔1-mic profile / coefficient / mask swap. Fixed-size
 * slots so there is no allocation; copy your POD param struct in/out. */
#ifndef HERMES_ABOX_PARAM_STORE_H
#define HERMES_ABOX_PARAM_STORE_H

#include <string.h>
#include "audio_core/abox/abox_atomic.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ABOX_PARAM_MAX 256   /* max bytes per parameter set */

typedef struct {
    unsigned char slot[2][ABOX_PARAM_MAX];
    int           size;
    _Atomic int   active;    /* index of the live slot */
} abox_param_store;

static inline void abox_param_init(abox_param_store* s, const void* init, int size) {
    s->size = size;
    atomic_store_explicit(&s->active, 0, memory_order_relaxed);
    if (init) {
        memcpy(s->slot[0], init, (size_t)size);
        memcpy(s->slot[1], init, (size_t)size);
    }
}

/* RT side: pointer to the currently-published parameters (acquire). Never blocks. */
static inline const void* abox_param_current(abox_param_store* s) {
    return s->slot[atomic_load_explicit(&s->active, memory_order_acquire)];
}

/* Control side: copy `value` into the inactive slot, then publish it atomically. The
 * previously-active slot stays valid until the next publish reuses it (double-buffer). */
static inline void abox_param_publish(abox_param_store* s, const void* value) {
    const int next = 1 - atomic_load_explicit(&s->active, memory_order_relaxed);
    memcpy(s->slot[next], value, (size_t)s->size);
    atomic_store_explicit(&s->active, next, memory_order_release);
}

#ifdef __cplusplus
}
#endif
#endif /* HERMES_ABOX_PARAM_STORE_H */
