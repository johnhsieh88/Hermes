/* abox_node.h — the C data-plane contract (SDD §2.1). Every DSP component implements
 * a uniform function-pointer vtable (abox_node_ops) over an opaque private state, so
 * the engine dispatches with one indirect call and zero RTTI. Frames are PLANAR views
 * (chan[c] points at engine-owned PCM), so the in-place cascade moves only pointers
 * (ABOX_XFER_INPLACE, zero-copy). C11; no malloc on the hot path. */
#ifndef HERMES_ABOX_NODE_H
#define HERMES_ABOX_NODE_H

#include <stdint.h>
#include <string.h>

#define ABOX_MAX_CHANNELS 2     /* 2-mic array */
#define ABOX_MAX_BLOCK    512   /* FFT-framing headroom over the 240-sample block */
#define ABOX_MAX_STAGES   8

#ifdef __cplusplus
extern "C" {
#endif

/* Engine use-case mode (SDS §3). Approach B: nodes adapt by mode on a STATIC graph. */
typedef enum {
    ABOX_MODE_KEYWORD_LISTENING = 0,   /* idle/wake — AEC bypassed */
    ABOX_MODE_BARGE_IN_MUTING   = 1,   /* user interrupted — ducking; AEC active */
    ABOX_MODE_CLOUD_STREAMING   = 2,   /* conversation — full duplex */
    ABOX_MODE_SYSTEM_RESET      = 3
} abox_mode;

/* Controllable graph elements (routing-matrix columns). */
typedef enum {
    ABOX_ELEM_SRC = 0, ABOX_ELEM_AEC, ABOX_ELEM_REF, ABOX_ELEM_BEAM,
    ABOX_ELEM_SES, ABOX_ELEM_CAPGATE, ABOX_ELEM_TTSOUT, ABOX_ELEM_COUNT
} abox_elem;

/* Hardware-agnostic planar frame view (the abox_frame). */
typedef struct {
    float*    chan[ABOX_MAX_CHANNELS];  /* per-channel sample pointers (zero-copy) */
    int       channels;
    int       frames;                   /* sample count this quantum */
    uint32_t  rate;
    uint64_t  sample_pos;               /* PipeWire clock.position timeline */
    abox_mode mode;                     /* latched per cycle (approach B) */
} abox_frame;

/* All conf params + shared services handed to every node at prepare() (the §4.0.1 g_ac). */
typedef struct {
    int   sample_rate;
    int   block_size;
    int   mic_channels;
    int   aec_partitions;
    int   aec_fft_size;
    float aec_mu;
    float aec_leak;
    int   ref_bulk_delay_max;
    int   beam_taps;
    int   target_azimuth;
} abox_config;

struct abox_node;

/* Explicit method vtable interface (the abox_node_ops). process() is in-place on io. */
typedef struct {
    void (*prepare)  (struct abox_node* n, const abox_config* cfg);
    void (*configure)(struct abox_node* n, uint32_t param_set_id);
    void (*process)  (struct abox_node* n, abox_frame* io);
    void (*reset)    (struct abox_node* n);
    void (*destroy)  (struct abox_node* n);
} abox_node_ops;

/* Monolithic node container instance. state = opaque historical loop memory. */
typedef struct abox_node {
    const abox_node_ops* ops;     /* immutable reference matrix (the vtable) */
    void*                state;   /* private per-instance DSP state */
    int                  in_ch;
    int                  out_ch;
} abox_node;

/* A stage = a node + the routing column whose mask bit gates it. */
typedef struct {
    abox_node* node;
    abox_elem  elem;
} abox_stage;

/* Complete route execution structure. */
typedef struct {
    abox_stage stages[ABOX_MAX_STAGES];
    int        count;
} abox_graph;

/* ── Routing matrix (abox_routing.c) ───────────────────────────────────────── */
uint32_t abox_active_mask(abox_mode m);              /* §10.9 active_pipeline_mask */
float    abox_route_gain (abox_mode m, abox_elem e);
static inline uint32_t abox_elem_bit(abox_elem e) { return (uint32_t)1u << (int)e; }

/* ── Graph (abox_graph.c) ──────────────────────────────────────────────────── */
void abox_graph_init(abox_graph* g);
int  abox_graph_add (abox_graph* g, abox_node* node, abox_elem elem);
/* Walk the static stage array; evaluate active_pipeline_mask in the tick loop and run
 * a stage iff its element bit is set for `mode`, else SKIP it (zero-copy — io already
 * holds the data, no realloc). Returns the number of stages executed this block. */
int  abox_graph_tick(abox_graph* g, abox_frame* io, abox_mode mode);

/* Soft-Mute Fallback (SDS §6.1/§10.5): zero-fill the frame — a clean mute beats a glitch
 * on a deadline overrun, and (unlike dropping input) keeps the AEC reference ring intact. */
static inline void abox_soft_mute(abox_frame* io) {
    for (int c = 0; c < io->channels && c < ABOX_MAX_CHANNELS; ++c)
        if (io->chan[c]) memset(io->chan[c], 0, sizeof(float) * (size_t)io->frames);
}

#ifdef __cplusplus
}
#endif
#endif /* HERMES_ABOX_NODE_H */
