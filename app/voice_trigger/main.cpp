// VTS (ModuleId 3) — always-on "Hey Aria" keyword detection.
// Own PipeWire raw-mic tap → sherpa-onnx keyword spotter (C API) +
// PrerollRing writer. On detection emits WAKE_CONFIRMED to Supervisor.
#include "audio_core/pipewire/Pw.hpp"
#include "hermes/common/Catalog.hpp"
#include "hermes/common/EventMap.hpp"
#include "hermes/common/ModuleId.hpp"
#include "hermes/common/MsgBus.hpp"
#include "hermes/common/PrerollRing.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef HERMES_HAVE_SHERPA_ONNX
#include <sherpa-onnx/c-api/c-api.h>
#endif

namespace hermes {

static constexpr uint32_t kMicRate   = 16000;
static constexpr int      kRingCap   = kMicRate * 4;  // 4 s audio ring (PCM f32)
static constexpr float    kWakeBoost = 1.5f;

static const char* kKwsDir =
    "/opt/ensoul/models/kws/sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01";
static const char* kKeywords = "hey aria @1.5\n";

// ── Simple SPSC float ring (PW → KWD thread) ──────────────────────────────────
struct FloatRing {
    static constexpr int kCap = kRingCap;
    float  buf[kCap]{};
    std::atomic<int> wp{0}, rp{0};

    int avail() const { return (wp.load() - rp.load() + kCap) % kCap; }

    void push(const float* src, int n) {
        int w = wp.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) {
            buf[w] = src[i];
            w = (w + 1) % kCap;
        }
        wp.store(w, std::memory_order_release);
    }

    int pop(float* dst, int n) {
        int av = avail();
        if (av < n) n = av;
        int r = rp.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) {
            dst[i] = buf[r];
            r = (r + 1) % kCap;
        }
        rp.store(r, std::memory_order_release);
        return n;
    }
};

// ── VoiceTrigger ──────────────────────────────────────────────────────────────
class VoiceTrigger : public MsgBus, public EventMap<VoiceTrigger> {
public:
    VoiceTrigger() {
        Add(_VoiceTrigger::cmd::ARM,           &VoiceTrigger::onArm);
        Add(_VoiceTrigger::cmd::DISARM,        &VoiceTrigger::onDisarm);
        Add(_VoiceTrigger::cmd::SET_THRESHOLD, &VoiceTrigger::onThreshold);
    }

    int ProcessMsg(CMsg* m) override { return Execute(m->hdr.id, m); }

    void Start() {
        armed_    = true;
        stopping_ = false;
        preroll_  = openPreroll();
        kwdThread_ = std::thread([this]{ kwdLoop(); });
        pwThread_  = std::thread([this]{ pwMain();  });
    }

    void Stop() {
        stopping_ = true;
        if (pwClient_) pwClient_->quit();
        if (pwThread_.joinable())  pwThread_.join();
        if (kwdThread_.joinable()) kwdThread_.join();
        if (preroll_) {
            munmap(preroll_, sizeof(PrerollRing));
            preroll_ = nullptr;
        }
    }

    void EmitWake(uint64_t wakePos, uint64_t fromPos, uint32_t epoch) {
        WakeConfirmedBody b{wakePos, fromPos, epoch};
        SendMsg(ModuleId::SUPERVISOR, _VoiceTrigger::evt::WAKE_CONFIRMED,
                PRIO_NORMAL, &b, sizeof b);
        fprintf(stderr, "[VTS] WAKE_CONFIRMED pos=%llu\n", (unsigned long long)wakePos);
    }

private:
    void onArm(const CMsg*)       { armed_ = true;  }
    void onDisarm(const CMsg*)    { armed_ = false; }
    void onThreshold(const CMsg*) {}

    // ── Open / create PrerollRing in shared memory ─────────────────────────────
    static PrerollRing* openPreroll() {
        int fd = shm_open("/hermes.preroll", O_CREAT | O_RDWR, 0600);
        if (fd < 0) { perror("shm_open /hermes.preroll"); return nullptr; }
        if (ftruncate(fd, sizeof(PrerollRing)) < 0) { perror("ftruncate"); close(fd); return nullptr; }
        void* p = mmap(nullptr, sizeof(PrerollRing), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (p == MAP_FAILED) { perror("mmap"); return nullptr; }
        return static_cast<PrerollRing*>(p);
    }

    // ── KWD thread: dequeue from FloatRing, run sherpa-onnx, detect "hey aria" ─
    void kwdLoop() {
#ifdef HERMES_HAVE_SHERPA_ONNX
        // Build config
        SherpaOnnxKeywordSpotterConfig cfg{};
        // Feature extraction
        cfg.feat_config.sample_rate     = (int)kMicRate;
        cfg.feat_config.feature_dim     = 80;
        // Transducer model
        char enc[512], dec[512], joi[512], tok[512];
        snprintf(enc, sizeof enc, "%s/encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx", kKwsDir);
        snprintf(dec, sizeof dec, "%s/decoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx", kKwsDir);
        snprintf(joi, sizeof joi, "%s/joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx",  kKwsDir);
        snprintf(tok, sizeof tok, "%s/tokens.txt", kKwsDir);
        cfg.model_config.transducer.encoder = enc;
        cfg.model_config.transducer.decoder = dec;
        cfg.model_config.transducer.joiner  = joi;
        cfg.model_config.tokens             = tok;
        cfg.model_config.num_threads        = 1;
        cfg.model_config.provider           = "cpu";
        cfg.max_active_paths                = 4;
        cfg.num_trailing_blanks             = 1;
        cfg.keywords_score                  = kWakeBoost;
        cfg.keywords_threshold              = 0.25f;
        cfg.keywords_buf                    = kKeywords;
        cfg.keywords_buf_size               = (int)strlen(kKeywords);

        const SherpaOnnxKeywordSpotter*  kws    = CreateKeywordSpotter(&cfg);
        SherpaOnnxOnlineStream*          stream = nullptr;

        if (!kws) {
            fprintf(stderr, "[VTS] sherpa-onnx KWS init failed — check model path %s\n", kKwsDir);
            goto fallback;
        }
        stream = CreateKeywordSpotterStream(kws);

        {
            static constexpr int kBatch = 1600;  // 100 ms @ 16kHz
            float tmp[kBatch];
            uint64_t samplePos = 0;

            while (!stopping_) {
                int got = audioRing_.pop(tmp, kBatch);
                if (got == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                SherpaOnnxOnlineStreamAcceptWaveform(stream, (int)kMicRate, tmp, got);
                while (IsKeywordSpotterReady(kws, stream)) {
                    DecodeKeywordStream(kws, stream);
                }
                const SherpaOnnxKeywordResult* r = GetKeywordResult(kws, stream);
                if (r && r->keyword && r->keyword[0] != '\0') {
                    if (armed_) {
                        uint64_t wakePos = samplePos;
                        uint64_t fromPos = wakePos > kMicRate * 3 ? wakePos - kMicRate * 3 : 0;
                        uint32_t epoch   = preroll_ ? preroll_->epoch.load() : 0;
                        EmitWake(wakePos, fromPos, epoch);
                    }
                    ClearKeywordSpotterResult(stream, kws);
                }
                samplePos += (uint64_t)got;
            }
        }

        DestroyOnlineStream(stream);
        DestroyKeywordSpotter(kws);
        return;
#endif
fallback:
        // Fallback: log and sleep (KWS unavailable — PTT via Supervisor still works)
        fprintf(stderr, "[VTS] KWD unavailable — compiled without sherpa-onnx or model missing\n");
        while (!stopping_) std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // ── PipeWire thread: raw mic tap ───────────────────────────────────────────
    void pwMain() {
        using namespace pw;
        pwClient_ = std::make_unique<PwClient>("hermes-voice-trigger");
        if (pwClient_->connect() < 0) {
            fprintf(stderr, "[VTS] PipeWire connect failed\n"); return;
        }
        micStream_ = std::make_unique<PwStream>(*pwClient_, "hermes-vts-mic",
                                                PwStream::CAPTURE, &VoiceTrigger::s_mic, this);
        micStream_->connect(kMicRate);
        pwClient_->run();
    }

    // PW capture callback — RT thread (no alloc, no blocking)
    static void s_mic(void* u, float* samples, uint32_t nf, uint32_t /*rate*/) {
        auto& self = *static_cast<VoiceTrigger*>(u);

        // Write to PrerollRing (int16)
        if (self.preroll_) {
            // Inline f32→s16 conversion into a small stack buffer
            static constexpr int kChunk = 512;
            int16_t tmp[kChunk];
            for (uint32_t i = 0; i < nf; i += kChunk) {
                uint32_t n = (nf - i < (uint32_t)kChunk) ? (nf - i) : (uint32_t)kChunk;
                for (uint32_t j = 0; j < n; ++j) {
                    float s = samples[i + j];
                    if (s >  1.0f) s =  1.0f;
                    if (s < -1.0f) s = -1.0f;
                    tmp[j] = (int16_t)(s * 32767.0f);
                }
                Preroll_Write(self.preroll_, tmp, (int)n);
            }
        }

        // Push float32 to KWD ring (best-effort; drop if ring full)
        self.audioRing_.push(samples, (int)nf);
    }

    // ── State ──────────────────────────────────────────────────────────────────
    std::atomic<bool>  armed_{true};
    std::atomic<bool>  stopping_{false};
    PrerollRing*       preroll_{nullptr};
    FloatRing          audioRing_;

    std::thread        kwdThread_;
    std::thread        pwThread_;
    std::unique_ptr<pw::PwClient> pwClient_;
    std::unique_ptr<pw::PwStream> micStream_;
};

} // namespace hermes

int main() {
    hermes::VoiceTrigger vts;
    vts.ConnectMsg(hermes::ModuleId::VOICE_TRIGGER);
    vts.Start();
    for (;;) pause();
    vts.Stop();
    return 0;
}
