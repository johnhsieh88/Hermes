#pragma once
#include <cstdint>
#include <cstring>

// DSP node framework (SDS §4.0). Multi-node deployment: each node is its own PipeWire
// filter and PipeWire links/schedules them — nodes do NOT call each other. Every node
// shares a common shape:
//   • common args: inCh / outCh, the input buffer + output buffer (process args),
//     and the AboxConfig (all conf params + shared services, given at prepare()).
//   • process(in,out): the node's DSP — invoked by PipeWire per quantum.
namespace hermes::dsp {

static constexpr int MAX_CHANNELS = 2;     // 2-mic array
static constexpr int MAX_BLOCK    = 512;   // FFT-framing headroom over the 240-sample block

// Engine use-case mode (SDS §3). Approach B: nodes adapt behavior by mode on a
// static graph (no re-routing). Carried per-block; latched coherently (ModeControl).
enum class EngineMode : int {
    KeywordListening = 0,   // idle/wake — no playback, AEC bypassed
    BargeInMuting    = 1,   // user interrupted — ducking; AEC active
    CloudStreaming   = 2,   // conversation — full duplex, AEC active
    SystemResetError = 3,
};

// Channel-major audio buffer (the inputBuf / outputBuf). Engine-owned, zero-alloc.
struct AudioBuffer {
    float      chan[MAX_CHANNELS][MAX_BLOCK];
    int        channelCount = 0;
    int        sampleCount  = 0;
    uint64_t   samplePos    = 0;                       // PipeWire timeline (clock.position)
    EngineMode mode         = EngineMode::KeywordListening;  // latched per cycle (approach B)
};

// All configuration parameters for the audio box + shared services (the SDS §4.0.1
// "g_ac"/AudioCommon). Built once at init, immutable thereafter → lock-free RT reads.
// Handed to every node at prepare(); add new common params/services here.
struct AboxConfig {
    // clock / framing
    int sampleRate  = 48000;
    int blockSize   = 240;       // 5 ms
    int micChannels = 2;
    // AEC (§4.3)
    int   aecPartitions   = 40;  // ~200 ms tail
    int   aecFftSize      = 512;
    float aecMu           = 0.1f;
    float aecLeak         = 1e-4f;
    int   refBulkDelayMax = 1536;
    // beamform (§4.2)
    int   beamTaps        = 32;
    int   targetAzimuth   = 0;
    // VAD / barge-in (§8)
    float vadSpeechFloor  = 0.0f;
    int   bargeDwellMs     = 50;
    // shared DSP kernels (NEON/SIMD) + facilities — filled at init
    void     (*fft)   (const float* in, float* out, int n)    = nullptr;
    void     (*ifft)  (const float* in, float* out, int n)    = nullptr;
    float    (*dotf)  (const float* a, const float* b, int n) = nullptr;
    void     (*biquad)(float* x, const float* coeffs, int n)  = nullptr;
    uint64_t (*now_ns)()                                      = nullptr;
};

class Node {
public:
    virtual ~Node() = default;
    virtual const char* name() const = 0;

    virtual void prepare(const AboxConfig& cfg) { cfg_ = &cfg; }   // common args for all nodes
    virtual void reset() {}

    // The node's DSP: consume `in`, produce `out`. Invoked by PipeWire per quantum
    // (PipeWire owns the graph links between nodes).
    virtual void process(const AudioBuffer& in, AudioBuffer& out) = 0;

    void setChannels(int ci, int co) { inCh_ = ci; outCh_ = co; }
    int  inChannels()  const         { return inCh_; }
    int  outChannels() const         { return outCh_; }

protected:
    const AboxConfig* cfg_   = nullptr;   // common config + services (valid after prepare)
    int               inCh_  = 0;
    int               outCh_ = 0;

    // BYPASS helper: identity passthrough (input → output), carrying mode.
    static void passthrough(const AudioBuffer& in, AudioBuffer& out) {
        out.channelCount = in.channelCount;
        out.sampleCount  = in.sampleCount;
        out.samplePos    = in.samplePos;
        out.mode         = in.mode;
        for (int c = 0; c < in.channelCount; ++c)
            std::memcpy(out.chan[c], in.chan[c], sizeof(float) * static_cast<size_t>(in.sampleCount));
    }
};

} // namespace hermes::dsp
