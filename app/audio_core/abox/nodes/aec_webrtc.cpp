/* aec_webrtc.cpp — AEC3 backend via libwebrtc-audio-processing-1.
 * Compiled only when HERMES_HAVE_WEBRTC_AP is defined (CMake detects the library).
 * Provides the same extern "C" symbols as aec_node.c; the CMakeLists includes exactly
 * one of them so there are no duplicate-symbol errors. */
#ifdef HERMES_HAVE_WEBRTC_AP

#include "audio_core/abox/abox_nodes.h"
#include "audio_core/abox/nodes/node_common.h"
#include "audio_core/abox/reference_manager.h"
#include "audio_core/abox/abox_node.h"

#include <modules/audio_processing/include/audio_processing.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ── Constants ─────────────────────────────────────────────────────────────────

// WebRTC APM requires 10 ms blocks; ABOX quantum is 240 samples (5 ms) at 48 kHz.
// We accumulate two ABOX blocks before each APM call and emit one half per call,
// introducing one-block (5 ms) output latency.
static constexpr int kApRate    = 48000;
static constexpr int kApBlock   = 480;         // 10 ms @ 48 kHz
static constexpr int kHalfBlock = kApBlock / 2; // 240 = one ABOX quantum

// ── State ─────────────────────────────────────────────────────────────────────

struct AecWebRtcState {
    rtc::scoped_refptr<webrtc::AudioProcessing> apm;
    webrtc::StreamConfig capture_cfg;
    webrtc::StreamConfig render_cfg;

    // Two-half accumulation buffers (480 samples each)
    float mic_acc[kApBlock];
    float ref_acc[kApBlock];
    int   acc_half;    // 0 or 1 — which 240-sample half we are currently filling

    // Output: after processing 480 samples, emit 240 per call (1-block delay)
    float out_buf[kApBlock];
    int   out_half;    // 0 or 1 — which half to emit this call
    bool  out_valid;   // true once the first APM block has been processed

    float          mix;
    abox_ref_manager* ref;
    float          ref_aligned[kApBlock]; // scratch for abox_ref_read_aligned
};

// ── Vtable implementations ─────────────────────────────────────────────────────

extern "C" {

static void aec_wrtc_prepare(abox_node* n, const abox_config* /*cfg*/) {
    auto* s = static_cast<AecWebRtcState*>(n->state);

    webrtc::AudioProcessing::Config cfg;
    cfg.echo_canceller.enabled       = true;
    cfg.echo_canceller.mobile_mode   = false;   // AEC3, not AECM
    cfg.noise_suppression.enabled    = true;
    cfg.noise_suppression.level      =
        webrtc::AudioProcessing::Config::NoiseSuppression::kModerate;
    cfg.gain_controller1.enabled     = false;   // AGC deferred to SES
    cfg.gain_controller2.enabled     = false;

    s->apm = webrtc::AudioProcessingBuilder().Create();
    if (!s->apm) { fprintf(stderr, "[AEC] WebRTC APM create failed\n"); return; }
    s->apm->ApplyConfig(cfg);

    s->capture_cfg = webrtc::StreamConfig(kApRate, 1);
    s->render_cfg  = webrtc::StreamConfig(kApRate, 1);
}

static void aec_wrtc_process(abox_node* n, abox_frame* io) {
    auto* s = static_cast<AecWebRtcState*>(n->state);
    if (!s->apm) return;   // APM failed to init — leave audio unchanged

    const int frames = io->frames;  // always kHalfBlock (240) while quantum is locked

    // Pull time-aligned far-end reference for this block from the ref_manager ring.
    if (s->ref)
        abox_ref_read_aligned(s->ref, s->ref_aligned, frames);
    else
        memset(s->ref_aligned, 0, frames * sizeof(float));

    // Accumulate into the 2-half buffers.
    const int off = s->acc_half * kHalfBlock;
    memcpy(s->mic_acc + off, io->chan[0],     frames * sizeof(float));
    memcpy(s->ref_acc + off, s->ref_aligned,  frames * sizeof(float));
    s->acc_half ^= 1;

    if (s->acc_half == 0) {
        // Both halves full → 480-sample block ready. Run APM.
        // ProcessReverseStream must be called before ProcessStream so AEC3
        // can compute the echo estimate for this render frame.
        const float* ref_in [1] = { s->ref_acc };
        float*       ref_out[1] = { s->ref_acc };
        s->apm->ProcessReverseStream(ref_in,  s->render_cfg,  s->render_cfg,  ref_out);

        const float* mic_in [1] = { s->mic_acc };
        float*       mic_out[1] = { s->mic_acc };
        s->apm->ProcessStream      (mic_in,  s->capture_cfg, s->capture_cfg, mic_out);

        memcpy(s->out_buf, s->mic_acc, kApBlock * sizeof(float));
        s->out_valid = true;
    }

    // Routing gain: 0 = bypass (KEYWORD_LISTENING / SYSTEM_RESET), 1 = full AEC.
    // Only emit processed output when the routing matrix says AEC is active;
    // leave io->chan[0] untouched otherwise so the pipeline still sees raw mic audio.
    if (s->out_valid && abox_route_gain(io->mode, ABOX_ELEM_AEC) > 0.0f) {
        memcpy(io->chan[0],
               s->out_buf + s->out_half * kHalfBlock,
               frames * sizeof(float));
        s->out_half ^= 1;
    }
    // out_half is NOT advanced during bypass so both halves are re-emittable once
    // the mode becomes active, avoiding a half-block gap at mode transitions.
}

static void aec_wrtc_reset(abox_node* n) {
    auto* s = static_cast<AecWebRtcState*>(n->state);
    if (s->apm) s->apm->Initialize();
    memset(s->mic_acc, 0, sizeof s->mic_acc);
    memset(s->ref_acc, 0, sizeof s->ref_acc);
    memset(s->out_buf, 0, sizeof s->out_buf);
    s->acc_half  = 0;
    s->out_half  = 0;
    s->out_valid = false;
    s->mix       = 0.0f;
}

static void aec_wrtc_destroy(abox_node* n) {
    if (!n) return;
    // state was C++-newed in abox_aec_create(); destroy properly.
    delete static_cast<AecWebRtcState*>(n->state);
    free(n);
}

static const abox_node_ops AEC_WEBRTC_OPS = {
    aec_wrtc_prepare,
    abox_node_default_configure,
    aec_wrtc_process,
    aec_wrtc_reset,
    aec_wrtc_destroy
};

// ── Public C API (same signatures as aec_node.c) ──────────────────────────────

abox_node* abox_aec_create(void) {
    // Allocate the node header as plain C struct (no constructor needed).
    abox_node* n = static_cast<abox_node*>(calloc(1, sizeof(abox_node)));
    if (!n) return nullptr;
    // Allocate state with C++ new so that scoped_refptr is properly constructed.
    n->state  = new AecWebRtcState{};
    n->ops    = &AEC_WEBRTC_OPS;
    n->in_ch  = 2;
    n->out_ch = 2;
    return n;
}

void abox_aec_set_ref(abox_node* n, abox_ref_manager* ref) {
    if (n && n->state)
        static_cast<AecWebRtcState*>(n->state)->ref = ref;
}

} // extern "C"

#endif // HERMES_HAVE_WEBRTC_AP
