#include "audio_core/abox/reference_manager.h"
#include <string.h>

void abox_ref_prepare(abox_ref_manager* r, int block) {
    r->block = block;
    abox_ref_reset(r);
}

void abox_ref_reset(abox_ref_manager* r) {
    memset(r->ring, 0, sizeof(r->ring));
    r->write_ptr = 0;
}

void abox_ref_set_bulk_delay(abox_ref_manager* r, int samples, float frac) {
    if (samples < 0) samples = 0;
    if (samples > ABOX_REF_BULK_DELAY_MAX - 2) samples = ABOX_REF_BULK_DELAY_MAX - 2;
    r->bulk_delay = samples;
    r->frac_delay = frac;
}

void abox_ref_write_farend(abox_ref_manager* r, const float* mixer, const float* vi_sense, int n) {
    for (int i = 0; i < n; ++i) {
        const float farend = vi_sense ? vi_sense[i] : (mixer ? mixer[i] : 0.0f);
        r->ring[r->write_ptr % ABOX_REF_BULK_DELAY_MAX] = farend;
        ++r->write_ptr;
    }
}

void abox_ref_read_aligned(const abox_ref_manager* r, float* out, int n) {
    for (int i = 0; i < n; ++i) {
        int64_t pos  = (int64_t)r->write_ptr - r->bulk_delay - (n - i);
        int64_t idx  = ((pos % ABOX_REF_BULK_DELAY_MAX) + ABOX_REF_BULK_DELAY_MAX) % ABOX_REF_BULK_DELAY_MAX;
        int64_t idx1 = (idx + 1) % ABOX_REF_BULK_DELAY_MAX;
        const float s0 = r->ring[idx];
        const float s1 = r->ring[idx1];
        out[i] = s0 + r->frac_delay * (s1 - s0);   /* linear sub-sample interpolation */
    }
}
