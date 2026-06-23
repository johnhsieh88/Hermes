#pragma once
#include "audio_core/dsp/Node.hpp"

namespace hermes::dsp {

// Channel-change node: converts channel count (SDS §14.3 matrix-mix). Common uses:
//   • stereo playback → MONO AEC reference (2→1 downmix, the stereo-AEC fix)
//   • mono TTS → stereo speakers (1→2 upmix)
//   • general N→M remap
// Modes: passthrough (N==M), downmix-to-mono (M==1, average), upmix-from-mono
// (N==1, copy), or an explicit mix matrix out[c] = Σ_k matrix[c][k]·in[k].
class ChannelMixNode : public Node {
public:
    explicit ChannelMixNode(int outChannels)
        : outCh_(outChannels < 1 ? 1 : (outChannels > MAX_CHANNELS ? MAX_CHANNELS : outChannels)) {}

    // Optional explicit mix matrix [outCh_][inCh]. If unset, the default down/up/
    // passthrough logic applies.
    void setMatrix(const float (*m)[MAX_CHANNELS], int outCh, int inCh) {
        haveMatrix_ = true; mOut_ = outCh; mIn_ = inCh;
        for (int o = 0; o < outCh && o < MAX_CHANNELS; ++o)
            for (int i = 0; i < inCh && i < MAX_CHANNELS; ++i) matrix_[o][i] = m[o][i];
    }

    const char* name() const override { return "ChannelMix"; }

    void process(const AudioBuffer& in, AudioBuffer& out) override {
        const int n = in.sampleCount;
        out.sampleCount = n;
        out.samplePos   = in.samplePos;
        out.mode        = in.mode;
        out.channelCount = haveMatrix_ ? mOut_ : outCh_;

        if (haveMatrix_) {                                   // explicit matrix mix
            for (int o = 0; o < out.channelCount; ++o)
                for (int i = 0; i < n; ++i) {
                    float s = 0.f;
                    for (int k = 0; k < mIn_ && k < in.channelCount; ++k)
                        s += matrix_[o][k] * in.chan[k][i];
                    out.chan[o][i] = s;
                }
            return;
        }
        if (in.channelCount == out.channelCount) {           // passthrough
            for (int c = 0; c < out.channelCount; ++c)
                std::memcpy(out.chan[c], in.chan[c], sizeof(float) * static_cast<size_t>(n));
        } else if (out.channelCount == 1) {                  // downmix N→1 (average)
            for (int i = 0; i < n; ++i) {
                float s = 0.f;
                for (int c = 0; c < in.channelCount; ++c) s += in.chan[c][i];
                out.chan[0][i] = s / static_cast<float>(in.channelCount);
            }
        } else if (in.channelCount == 1) {                   // upmix 1→N (copy)
            for (int c = 0; c < out.channelCount; ++c)
                std::memcpy(out.chan[c], in.chan[0], sizeof(float) * static_cast<size_t>(n));
        } else {                                             // N→M: copy/clamp per channel
            for (int c = 0; c < out.channelCount; ++c) {
                int src = (c < in.channelCount) ? c : in.channelCount - 1;
                std::memcpy(out.chan[c], in.chan[src], sizeof(float) * static_cast<size_t>(n));
            }
        }
    }

private:
    int   outCh_;
    bool  haveMatrix_ = false;
    int   mOut_ = 0, mIn_ = 0;
    float matrix_[MAX_CHANNELS][MAX_CHANNELS] = {};
};

} // namespace hermes::dsp
