/* reference_manager.h — AEC Reference / Loopback path (SDS §4.3.1–§4.3.2). Taps the
 * post-fader mixer output AND the smart-amp VI-Sense feedback into a delay ring, then
 * emits a sample-aligned far-end frame for the apm_aec node: a cheap integer BULK delay
 * (ring read offset) removes transport delay D, a fractional residual tracks drift. The
 * tap is POST-FADER so a barge-in duck is reflected and AEC never over-subtracts. */
#ifndef HERMES_ABOX_REFERENCE_MANAGER_H
#define HERMES_ABOX_REFERENCE_MANAGER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ABOX_REF_BULK_DELAY_MAX 1536   /* ≥ worst-case transport delay (~96 ms @ 16 kHz) */

typedef struct {
    float    ring[ABOX_REF_BULK_DELAY_MAX];   /* POST-FADER far-end ring (what was emitted) */
    uint32_t write_ptr;
    int      bulk_delay;                       /* D_bulk — integer ref delay count */
    float    frac_delay;                       /* sub-sample residual (drift) */
    int      block;
} abox_ref_manager;

void abox_ref_prepare(abox_ref_manager* r, int block);
void abox_ref_reset(abox_ref_manager* r);
void abox_ref_set_bulk_delay(abox_ref_manager* r, int samples, float frac);   /* §4.3.2c seed */

/* Tap one block of post-fader far-end. vi_sense (smart-amp feedback, the truest emitted
 * signal) is preferred per sample when non-NULL, else the digital mixer output is used. */
void abox_ref_write_farend(abox_ref_manager* r, const float* mixer, const float* vi_sense, int n);

/* Produce the time-aligned reference for the AEC block of n samples just written.
 *   idx = (write_ptr − bulk_delay − (n − i)) mod MAX;  out[i] = interp(ring[idx], frac) */
void abox_ref_read_aligned(const abox_ref_manager* r, float* out, int n);

#ifdef __cplusplus
}
#endif
#endif /* HERMES_ABOX_REFERENCE_MANAGER_H */
