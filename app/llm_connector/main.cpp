// LLM_CONNECTOR (ModuleId 5) — on-target AI proxy.
// PipeWire capture (abox out_0 clean mono when HERMES_PW_CAP_TARGET=hermes.abox, else the
// raw default mic) → AudioRing (RT→worker seam) → resident streaming STT (sherpa-onnx C API;
// subprocess fallback without it) → amplitude-VAD endpoint → LLM (Groq via libcurl) →
// TTS (Piper subprocess) → PipeWire playback.
#include "audio_core/pipewire/Pw.hpp"
#include "hermes/common/AudioRing.hpp"
#include "hermes/common/Catalog.hpp"
#include "hermes/common/Log.h"
#include "hermes/common/EventMap.hpp"
#include "hermes/common/ModuleId.hpp"
#include "hermes/common/MsgBus.hpp"
#include "hermes/common/PrerollRing.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#ifdef HERMES_HAVE_CURL
#include <curl/curl.h>
#endif
#ifdef HERMES_HAVE_SHERPA_ONNX
#include <sherpa-onnx/c-api/c-api.h>
#endif

namespace hermes {

static constexpr uint32_t kCaptureRate  = 48000;  // out_0 native contract (ARCHITECTURE §13.2);
                                                  // sherpa resamples to its 16 k features itself
static constexpr float    kSilenceRMS   = 0.008f;
static constexpr int      kSpeechMinMs  = 400;
static constexpr int      kSilenceMs    = 900;
static constexpr int      kMaxUtterMs   = 20000;

static const char* kSttBase  = "/opt/ensoul/models/stt/"
                                "sherpa-onnx-streaming-zipformer-en-20M-2023-02-17";
static const char* kSecretsFile = "/etc/anime-ai/secrets.env";
static const char* kGroqUrl  = "https://api.groq.com/openai/v1/chat/completions";
static const char* kGroqModel = "llama-3.1-8b-instant";
static const char* kSystemPrompt =
    "You are Aria, a friendly and expressive anime-style AI desk companion. "
    "Respond warmly and concisely in 1-3 sentences.";

// ── Helpers ───────────────────────────────────────────────────────────────────

static void write_wav(const char* path, const std::vector<int16_t>& pcm, uint32_t rate) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    uint32_t dataSz = (uint32_t)(pcm.size() * 2);
    uint32_t riffSz = dataSz + 36;
    uint16_t ch = 1, bits = 16, blk = 2; uint32_t brate = rate * 2;
    fwrite("RIFF", 1, 4, f); fwrite(&riffSz, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    uint32_t fsz = 16; uint16_t pcmFmt = 1;
    fwrite(&fsz, 4, 1, f); fwrite(&pcmFmt, 2, 1, f); fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&brate, 4, 1, f); fwrite(&blk, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dataSz, 4, 1, f);
    fwrite(pcm.data(), 2, pcm.size(), f);
    fclose(f);
}

struct WavPcm { uint32_t rate = 0; std::vector<float> f32; };

static WavPcm load_wav(const char* path) {
    WavPcm out;
    FILE* f = fopen(path, "rb");
    if (!f) return out;
    uint8_t hdr[44]; fread(hdr, 1, 44, f);
    out.rate = *(uint32_t*)(hdr + 24);
    uint32_t dataSz = *(uint32_t*)(hdr + 40);
    std::vector<int16_t> s16(dataSz / 2);
    fread(s16.data(), 2, s16.size(), f);
    fclose(f);
    out.f32.resize(s16.size());
    for (size_t i = 0; i < s16.size(); ++i)
        out.f32[i] = s16[i] / 32768.0f;
    return out;
}

static std::string read_key(const char* envVar) {
    const char* v = getenv(envVar);
    if (v && *v) return v;
    FILE* f = fopen(kSecretsFile, "r");
    if (!f) return {};
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, envVar, strlen(envVar)) == 0 && line[strlen(envVar)] == '=') {
            fclose(f);
            std::string val = line + strlen(envVar) + 1;
            while (!val.empty() && (val.back() == '\n' || val.back() == '\r'))
                val.pop_back();
            return val;
        }
    }
    fclose(f);
    return {};
}

// ── STT ───────────────────────────────────────────────────────────────────────
static std::string run_stt(const std::vector<int16_t>& pcm, uint32_t rate) {
    // Test bypass: set HERMES_TEST_UTTERANCE to skip sherpa-onnx inference.
    const char* stub = getenv("HERMES_TEST_UTTERANCE");
    if (stub && *stub) {
        fprintf(stderr, "[CC] STT stub: '%s'\n", stub);
        return stub;
    }
    static const char* kWav = "/tmp/cc-utterance.wav";
    write_wav(kWav, pcm, rate);
    char cmd[2048];
    snprintf(cmd, sizeof cmd,
        "sherpa-onnx"
        " --encoder=%s/encoder-epoch-99-avg-1.int8.onnx"
        " --decoder=%s/decoder-epoch-99-avg-1.int8.onnx"
        " --joiner=%s/joiner-epoch-99-avg-1.int8.onnx"
        " --tokens=%s/tokens.txt"
        " --num-threads=2"
        " %s 2>&1",
        kSttBase, kSttBase, kSttBase, kSttBase, kWav);
    FILE* p = popen(cmd, "r");
    if (!p) { unlink(kWav); return {}; }
    char buf[4096] = {};
    fread(buf, 1, sizeof buf - 1, p);
    pclose(p);
    unlink(kWav);
    const char* needle = "\"text\": \"";
    char* start = strstr(buf, needle);
    if (!start) return {};
    start += strlen(needle);
    std::string text;
    for (; *start && *start != '"'; ++start) {
        if (*start == '\\' && *(start+1)) { ++start; }
        text += *start;
    }
    return text;
}

// ── Groq LLM ──────────────────────────────────────────────────────────────────
#ifdef HERMES_HAVE_CURL
static size_t curl_sink(void* ptr, size_t sz, size_t n, std::string* out) {
    out->append(static_cast<char*>(ptr), sz * n); return sz * n;
}

static void json_escape(const std::string& s, std::string& out) {
    for (char c : s) {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else                out += c;
    }
}

static std::string groq_chat(
    const std::string& key,
    const std::string& userText,
    const std::vector<std::pair<std::string,std::string>>& history)   // read-only: the caller
{                                                                     // owns history updates
    std::string msgs = "[{\"role\":\"system\",\"content\":\"";
    json_escape(kSystemPrompt, msgs); msgs += "\"}";
    for (auto& [role, content] : history) {
        msgs += ",{\"role\":\"" + role + "\",\"content\":\"";
        json_escape(content, msgs); msgs += "\"}";
    }
    msgs += ",{\"role\":\"user\",\"content\":\"";
    json_escape(userText, msgs); msgs += "\"}]";
    std::string body = "{\"model\":\"" + std::string(kGroqModel) + "\","
                       "\"messages\":" + msgs + "}";

    CURL* curl = curl_easy_init();
    if (!curl) return {};
    std::string resp;
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, ("Authorization: Bearer " + key).c_str());
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL,           kGroqUrl);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_sink);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    const char* needle = "\"content\":\"";
    const char* p = strstr(resp.c_str(), needle);
    if (!p) return {};
    p += strlen(needle);
    std::string reply;
    for (; *p && *p != '"'; ++p) {
        if (*p == '\\' && *(p+1)) {
            ++p;
            if (*p == 'n') reply += '\n';
            else reply += *p;
            continue;
        }
        reply += *p;
    }
    return reply;
}
#else
static std::string groq_chat(
    const std::string&, const std::string& userText,
    const std::vector<std::pair<std::string,std::string>>&)
{
    fprintf(stderr, "[CC] libcurl not compiled in — echo: %s\n", userText.c_str());
    return "I heard you say: " + userText;
}
#endif

// ── TTS (Piper subprocess) ────────────────────────────────────────────────────
static WavPcm run_tts(const std::string& text) {
    // Test bypass: skip Piper and return 0.5s of silence at 22050 Hz.
    if (getenv("HERMES_TEST_UTTERANCE") && *getenv("HERMES_TEST_UTTERANCE")) {
        fprintf(stderr, "[CC] TTS stub: generating 0.5s silence for '%s'\n", text.c_str());
        WavPcm out; out.rate = 22050;
        out.f32.assign(22050 / 2, 0.0f);  // 0.5s silence
        return out;
    }
    static const char* kWav   = "/tmp/cc-tts.wav";
    static const char* kModel = "/opt/ensoul/models/tts/en_US-hfc_female-medium.onnx";
    FILE* p = popen(
        (std::string("piper --model ") + kModel +
         " --output_file " + kWav +
         " --sentence_silence 0.1 2>/dev/null").c_str(), "w");
    if (!p) return {};
    fwrite(text.c_str(), 1, text.size(), p);
    pclose(p);
    WavPcm wav = load_wav(kWav);
    unlink(kWav);
    return wav;
}

// ── LlmConnector ────────────────────────────────────────────────────────────
class LlmConnector : public MsgBus, public EventMap<LlmConnector> {
public:
    LlmConnector() {
        Add(_Llm::cmd::OPEN_STREAM,   &LlmConnector::onOpen);
        Add(_Llm::cmd::CLOSE_STREAM,  &LlmConnector::onClose);
        Add(_Llm::cmd::UTTERANCE_END, &LlmConnector::onUttEnd);
        Add(_Llm::cmd::ABORT,         &LlmConnector::onAbort);
        apiKey_ = read_key("GROQ_API_KEY");
    }

    int ProcessMsg(CMsg* m) override { return Execute(m->hdr.id, m); }

    void Start() {
        stopping_ = false;
        initStt();                                     // resident recognizer — loads once here,
        sttThread_ = std::thread([this]{ sttLoop(); });//   never per-utterance
        monThread_ = std::thread([this]{ monLoop(); });
        vadThread_ = std::thread([this]{ vadLoop(); });
        pwThread_  = std::thread([this]{ pwMain();  });
    }

    void Stop() {
        stopping_ = true;
        sttCv_.notify_all();
        if (pwClient_) pwClient_->quit();
        if (pwThread_.joinable())  pwThread_.join();
        if (vadThread_.joinable()) vadThread_.join();
        if (monThread_.joinable()) monThread_.join();
        if (sttThread_.joinable()) sttThread_.join();
        destroyStt();
    }

private:
    void onOpen(const CMsg* m) {
        // No ring_.drain() here — rp_ is sttLoop's alone (AudioRing SPSC contract). Stale
        // idle audio never accumulates anyway: sttLoop discards pops that arrive outside a
        // turn (see the feed condition there).
        { std::lock_guard<std::mutex> lk(pcmMtx_);
          ++turnGen_;                                   // stale pipeline()/finalize become no-ops
          pcmBuf_.clear(); sttResult_.clear(); sttDone_ = false;
          backfill_.clear(); }
        backfillPending_ = false;
        // PREROLL BACKFILL (§16.3): a wake-entry OPEN_STREAM carries WakeConfirmedBody —
        // splice the VTS ring history (what the child said OVER the keyword) in front of
        // the live stream so the utterance is gapless from its true start.
        if (m && m->pBody && m->hdr.length >= sizeof(WakeConfirmedBody) && sttResident())
            scheduleBackfill(*static_cast<const WakeConfirmedBody*>(m->pBody));
        finalizeGen_ = 0;
        turnSamples_ = 0;
        vadSpeechMs_ = 0; vadSilenceMs_ = 0;
        vadHadSpeech_ = false; vadEndSent_ = false;
        abort_ = false;
        capturing_ = true;
    }
    void onClose(const CMsg*)  { capturing_ = false; }
    void onUttEnd(const CMsg*) {
        capturing_ = false;
        finalizeGen_ = turnGen_.load();                 // tag the finalize with ITS turn —
        std::thread([this]{ pipeline(); }).detach();    //   sttLoop drops it if a newer turn opened
    }
    void onAbort(const CMsg*) {
        abort_ = true; capturing_ = false; playing_ = false;
        sttCv_.notify_all();                            // release a pipeline() waiting on finalize
    }

    // ── Resident streaming STT (sherpa-onnx C API; loads once at Start()) ────
    void initStt() {
#ifdef HERMES_HAVE_SHERPA_ONNX
        char enc[512], dec[512], joi[512], tok[512];
        snprintf(enc, sizeof enc, "%s/encoder-epoch-99-avg-1.int8.onnx", kSttBase);
        snprintf(dec, sizeof dec, "%s/decoder-epoch-99-avg-1.int8.onnx", kSttBase);
        snprintf(joi, sizeof joi, "%s/joiner-epoch-99-avg-1.int8.onnx",  kSttBase);
        snprintf(tok, sizeof tok, "%s/tokens.txt", kSttBase);
        SherpaOnnxOnlineRecognizerConfig cfg{};
        cfg.feat_config.sample_rate         = 16000;   // model feature rate — input arrives at
        cfg.feat_config.feature_dim         = 80;      //   48 k, sherpa resamples internally
        cfg.model_config.transducer.encoder = enc;
        cfg.model_config.transducer.decoder = dec;
        cfg.model_config.transducer.joiner  = joi;
        cfg.model_config.tokens             = tok;
        cfg.model_config.num_threads        = 2;
        cfg.model_config.provider           = "cpu";
        cfg.decoding_method                 = "greedy_search";
        rec_ = SherpaOnnxCreateOnlineRecognizer(&cfg);
        if (rec_) sttStream_ = SherpaOnnxCreateOnlineStream(rec_);
        fprintf(stderr, rec_ ? "[CC] resident STT ready (%s)\n"
                             : "[CC] resident STT init FAILED (%s) — subprocess fallback\n",
                kSttBase);
#endif
    }
    void destroyStt() {
#ifdef HERMES_HAVE_SHERPA_ONNX
        if (sttStream_) { SherpaOnnxDestroyOnlineStream(sttStream_); sttStream_ = nullptr; }
        if (rec_)       { SherpaOnnxDestroyOnlineRecognizer(rec_);   rec_ = nullptr; }
#endif
    }
    bool sttResident() const {
#ifdef HERMES_HAVE_SHERPA_ONNX
        return rec_ != nullptr && sttStream_ != nullptr;
#else
        return false;
#endif
    }

    // Map VTS's preroll ring read-only (lazy, once). Absent SHM → backfill silently off.
    PrerollRing* openPreroll() {
        if (preroll_) return preroll_;
        int fd = shm_open("/hermes.preroll", O_RDONLY, 0);
        if (fd < 0) return nullptr;
        void* pmap = mmap(nullptr, sizeof(PrerollRing), PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (pmap == MAP_FAILED) return nullptr;
        preroll_ = static_cast<PrerollRing*>(pmap);
        return preroll_;
    }

    // Copy the [from,to) window (int16 @16 k, VTS position domain) into backfill_ as float.
    // Runs on the recv thread (non-RT) at turn start; sttLoop feeds it to the recognizer.
    void scheduleBackfill(const WakeConfirmedBody& w) {
        PrerollRing* r = openPreroll();
        if (!r) { HM_LOG_DEBUG("preroll: SHM absent — no backfill"); return; }
        uint64_t from = 0, to = 0;
        if (!Preroll_Window(r, w.capture_from_pos, w.epoch, &from, &to) || to <= from) {
            HM_LOG_WARN("preroll: window invalid (epoch mismatch/xrun) — live-only turn");
            return;
        }
        std::lock_guard<std::mutex> lk(pcmMtx_);
        backfill_.resize(static_cast<size_t>(to - from));
        for (uint64_t i = from; i < to; ++i)
            backfill_[static_cast<size_t>(i - from)] =
                r->pcm[i % PREROLL_RING_SAMPLES] / 32768.0f;
        backfillPending_ = true;
        HM_LOG_INFO("preroll: backfill %llu ms queued (wake_pos=%llu epoch=%u)",
                    (unsigned long long)((to - from) / 16),      // 16 kHz ring
                    (unsigned long long)w.wake_pos, w.epoch);
    }

    // Consumer side of ring_ (single consumer): feed the resident recognizer in ~100 ms
    // chunks; without sherpa, accumulate int16 for the run_stt() subprocess fallback.
    // On finalizeReq_ (endpoint) with the ring drained, produce the final transcript.
    void sttLoop() {
        float chunk[4800];                               // 100 ms @ 48 k
        while (!stopping_) {
            if (backfillPending_.exchange(false)) {      // ring history BEFORE live audio
#ifdef HERMES_HAVE_SHERPA_ONNX
                std::vector<float> bf;
                { std::lock_guard<std::mutex> lk(pcmMtx_); bf = std::move(backfill_); backfill_.clear(); }
                if (sttResident() && !bf.empty()) {
                    SherpaOnnxOnlineStreamAcceptWaveform(sttStream_, 16000,   // ring is 16 k
                                                         bf.data(), (int)bf.size());
                    while (SherpaOnnxIsOnlineStreamReady(rec_, sttStream_))
                        SherpaOnnxDecodeOnlineStream(rec_, sttStream_);
                    turnSamples_.fetch_add(bf.size() * 3, std::memory_order_relaxed); // 48k-equiv
                }
#endif
            }
            int n = ring_.pop(chunk, 4800);
            if (n > 0) {
                // Feed only turn audio: while capturing, or draining the tail toward a
                // pending finalize. Outside a turn any popped data is stale (e.g. the one
                // in-flight block that landed after the endpoint) — discard it.
                if (!capturing_.load(std::memory_order_relaxed) && finalizeGen_.load() == 0)
                    continue;
                turnSamples_.fetch_add((uint64_t)n, std::memory_order_relaxed);
                if (sttResident()) {
#ifdef HERMES_HAVE_SHERPA_ONNX
                    SherpaOnnxOnlineStreamAcceptWaveform(sttStream_, (int)kCaptureRate, chunk, n);
                    while (SherpaOnnxIsOnlineStreamReady(rec_, sttStream_))
                        SherpaOnnxDecodeOnlineStream(rec_, sttStream_);
#endif
                } else {
                    std::lock_guard<std::mutex> lk(pcmMtx_);
                    for (int i = 0; i < n; ++i) {
                        float s = chunk[i];
                        s = s >  1.0f ?  1.0f : s;
                        s = s < -1.0f ? -1.0f : s;
                        pcmBuf_.push_back(static_cast<int16_t>(s * 32767.0f));
                    }
                }
                continue;                                // keep draining while data flows
            }
            if (uint32_t g = finalizeGen_.exchange(0)) { // ring empty + endpoint → finalize
                std::string text;
                if (sttResident()) {
#ifdef HERMES_HAVE_SHERPA_ONNX
                    SherpaOnnxOnlineStreamInputFinished(sttStream_);
                    while (SherpaOnnxIsOnlineStreamReady(rec_, sttStream_))
                        SherpaOnnxDecodeOnlineStream(rec_, sttStream_);
                    const SherpaOnnxOnlineRecognizerResult* r =
                        SherpaOnnxGetOnlineStreamResult(rec_, sttStream_);
                    if (r) {
                        if (r->text) text = r->text;
                        SherpaOnnxDestroyOnlineRecognizerResult(r);
                    }
                    SherpaOnnxOnlineStreamReset(rec_, sttStream_);   // fresh stream, next turn
#endif
                }
                HM_LOG_INFO("turn finalized: %llu ms audio decoded | ring hw=%d ms over=%llu | text %zu B",
                            (unsigned long long)(turnSamples_.load(std::memory_order_relaxed) / 48),
                            ring_.highWater() / 48,
                            (unsigned long long)ring_.overruns(),
                            text.size());
                {
                    std::lock_guard<std::mutex> lk(pcmMtx_);
                    if (turnGen_.load() == g) {          // only publish into the SAME turn —
                        sttResult_ = std::move(text);    //   a newer onOpen makes this a no-op
                        sttDone_ = true;                 //   (stale transcript can't leak)
                    }
                }
                sttCv_.notify_all();
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    void vadLoop() {
        int logTick = 0;
        while (!stopping_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (++logTick >= 40) {  // every 2s — the buffering health line
                logTick = 0;
                const int avail = ring_.available();
                HM_LOG_DEBUG("vad: capturing=%d rms=%.4f calls=%llu | ring %d smp (%d ms, %d%% of %d ms cap) hw=%d ms over=%llu",
                             (int)capturing_.load(),
                             rms_.load(std::memory_order_relaxed),
                             (unsigned long long)captureCallCount_.load(),
                             avail, avail / 48,
                             avail * 100 / ring_.capacity(),
                             ring_.capacity() / 48,
                             ring_.highWater() / 48,
                             (unsigned long long)ring_.overruns());
            }
            if (!capturing_ || vadEndSent_) continue;
            float rms = rms_.load(std::memory_order_relaxed);
            if (rms > kSilenceRMS) {
                vadSpeechMs_ += 50; vadSilenceMs_ = 0;
                if (vadSpeechMs_ >= kSpeechMinMs) vadHadSpeech_ = true;
            } else {
                vadSilenceMs_ += 50;
                if (vadHadSpeech_ && vadSilenceMs_ >= kSilenceMs) {
                    vadEndSent_ = true;
                    SendMsg(ModuleId::SUPERVISOR, _Llm::evt::STT_ENDPOINT, PRIO_NORMAL);
                }
            }
            if (vadSpeechMs_ + vadSilenceMs_ >= kMaxUtterMs && !vadEndSent_) {
                vadEndSent_ = true;
                SendMsg(ModuleId::SUPERVISOR, _Llm::evt::STT_ENDPOINT, PRIO_NORMAL);
            }
        }
    }

    void pipeline() {
        // One detached thread per turn. myGen makes a superseded thread inert: every stage
        // gate checks it, and no shared state (sttResult_/history_/ttsWav_) is touched once
        // a newer onOpen() has bumped turnGen_.
        const uint32_t myGen = turnGen_.load();
        // Transcript source, in priority order: test stub → resident STT (sttLoop finalize
        // handshake) → run_stt() subprocess fallback on the pcm sttLoop accumulated.
        std::string transcript;
        std::vector<int16_t> pcm;
        const char* stub = getenv("HERMES_TEST_UTTERANCE");
        if (stub && *stub) {
            fprintf(stderr, "[CC] STT stub: '%s'\n", stub);
            transcript = stub;
        } else {
            std::unique_lock<std::mutex> lk(pcmMtx_);
            // QEMU decodes ~4.5× slower than real time — allow the backlog to drain.
            sttCv_.wait_for(lk, std::chrono::seconds(60),
                            [&]{ return sttDone_ || abort_.load() || stopping_.load()
                                     || turnGen_.load() != myGen; });
            if (stopping_.load() || turnGen_.load() != myGen) return;   // shutdown / superseded
            transcript = std::move(sttResult_); sttResult_.clear(); sttDone_ = false;
            pcm = std::move(pcmBuf_); pcmBuf_.clear();
        }
        if (abort_) { abort_ = false; return; }
        if (transcript.empty() && !pcm.empty())
            transcript = run_stt(pcm, kCaptureRate);         // no-sherpa fallback path
        fprintf(stderr, "[CC] transcript: '%s'\n", transcript.c_str());
        if (transcript.empty() || abort_) {
            SendMsg(ModuleId::SUPERVISOR, _Llm::evt::STT_NO_SPEECH, PRIO_NORMAL);
            abort_ = false; return;
        }
        SendMsg(ModuleId::STORY_AGENT, _Llm::evt::STT_FINAL, PRIO_NORMAL,
                transcript.c_str(), (uint32_t)transcript.size());   // §16.2: story_agent consumes
        if (abort_ || turnGen_.load() != myGen) { abort_ = false; return; }

        // history_ is copied under the lock for the request, and updated under the lock
        // (gen-gated) after — groq_chat itself no longer mutates shared state, so a stale
        // thread stuck in curl can never corrupt the vector a newer turn is using.
        std::vector<std::pair<std::string, std::string>> hist;
        { std::lock_guard<std::mutex> lk(pcmMtx_); hist = history_; }
        std::string reply = groq_chat(apiKey_, transcript, hist);
        fprintf(stderr, "[CC] reply: %s\n", reply.c_str());
        if (reply.empty() || abort_) {
            SendMsg(ModuleId::SUPERVISOR, _Llm::evt::LLM_ERROR, PRIO_NORMAL);
            abort_ = false; return;
        }
        {
            std::lock_guard<std::mutex> lk(pcmMtx_);
            if (turnGen_.load() == myGen) {
                history_.push_back({"user", transcript});
                history_.push_back({"assistant", reply});
                if (history_.size() > 20) history_.erase(history_.begin(), history_.begin() + 2);
            }
        }

        WavPcm wav = run_tts(reply);
        if (wav.f32.empty() || abort_) { abort_ = false; return; }

        {   // install playback only if this is still the live turn (onOpen holds the same
            // lock while bumping turnGen_, so a stale install can't slip past the check)
            std::lock_guard<std::mutex> lk(pcmMtx_);
            if (turnGen_.load() != myGen) return;
            ttsWav_  = std::move(wav.f32);
            ttsRate_ = wav.rate;
            ttsOffset_.store(0, std::memory_order_relaxed);
        }
        SendMsg(ModuleId::SUPERVISOR, _Llm::evt::TTS_CHUNK,      PRIO_NORMAL);
        SendMsg(ModuleId::SUPERVISOR, _Llm::evt::TTS_STREAM_END,  PRIO_NORMAL);
        playing_.store(true, std::memory_order_release);
        abort_ = false;
    }

    void monLoop() {
        while (!stopping_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (drainPending_.exchange(false))
                SendMsg(ModuleId::SUPERVISOR, _AudioCore::evt::PLAYBACK_DRAINED, PRIO_NORMAL);
        }
    }

    void pwMain() {
        using namespace pw;
        pwClient_ = std::make_unique<PwClient>("hermes-cloud-connector");
        if (pwClient_->connect() < 0) {
            fprintf(stderr, "[CC] PipeWire connect failed\n"); return;
        }
        // Allow overriding PW targets via env for QEMU test environments where
        // the virtio ALSA card is suspended. On real hardware, leave unset.
        const char* capTarget  = getenv("HERMES_PW_CAP_TARGET");
        const char* playTarget = getenv("HERMES_PW_PLAY_TARGET");
        capStream_  = std::make_unique<PwStream>(*pwClient_, "hermes-cc-mic",
                                                 PwStream::CAPTURE,  &LlmConnector::s_capture,  this,
                                                 capTarget);
        playStream_ = std::make_unique<PwStream>(*pwClient_, "hermes-cc-tts",
                                                 PwStream::PLAYBACK, &LlmConnector::s_playback, this,
                                                 playTarget);
        capStream_->connect(kCaptureRate);
        playStream_->connect(22050);
        pwClient_->run();
    }

    // PW RT thread — one lock-free push, nothing else (no mutex, no alloc on the RT path).
    static void s_capture(void* u, float* samples, uint32_t nf, uint32_t) {
        auto& self = *static_cast<LlmConnector*>(u);
        self.captureCallCount_.fetch_add(1, std::memory_order_relaxed);
        float rms = 0.0f;
        for (uint32_t i = 0; i < nf; ++i) rms += samples[i] * samples[i];
        self.rms_.store(std::sqrt(rms / (float)nf), std::memory_order_relaxed);
        if (!self.capturing_.load(std::memory_order_relaxed)) return;
        self.ring_.push(samples, (int)nf);   // drop-new + counter on overflow, never blocks
    }

    static void s_playback(void* u, float* samples, uint32_t nf, uint32_t) {
        auto& self = *static_cast<LlmConnector*>(u);
        if (!self.playing_.load(std::memory_order_acquire)) {
            std::memset(samples, 0, nf * sizeof(float)); return;
        }
        size_t off = self.ttsOffset_.load(std::memory_order_relaxed);
        size_t rem = self.ttsWav_.size() - off;
        uint32_t toCopy = (rem < nf) ? (uint32_t)rem : nf;
        std::memcpy(samples, self.ttsWav_.data() + off, toCopy * sizeof(float));
        if (toCopy < nf) std::memset(samples + toCopy, 0, (nf - toCopy) * sizeof(float));
        self.ttsOffset_.store(off + toCopy, std::memory_order_release);
        if (off + toCopy >= self.ttsWav_.size())
            self.drainPending_.store(true, std::memory_order_release);
    }

    std::atomic<bool>    capturing_{false};
    std::atomic<bool>    abort_{false};
    std::atomic<float>   rms_{0.0f};
    std::atomic<uint64_t> captureCallCount_{0};

    AudioRing<96000>     ring_;               // 2 s @ 48 k — the RT-capture → sttLoop seam
#ifdef HERMES_HAVE_SHERPA_ONNX
    const SherpaOnnxOnlineRecognizer* rec_{nullptr};
    const SherpaOnnxOnlineStream*     sttStream_{nullptr};
#endif
    std::mutex           pcmMtx_;             // guards pcmBuf_/sttResult_/sttDone_ (worker side
    std::vector<int16_t> pcmBuf_;             //   only — the RT callback never takes it now)
    std::condition_variable sttCv_;           // finalize handshake: sttLoop → pipeline()
    std::atomic<uint32_t> turnGen_{0};        // bumped by onOpen (under pcmMtx_); stale turns'
    std::atomic<uint32_t> finalizeGen_{0};    //   threads/finalizes gate on it. 0 = no finalize
    std::atomic<uint64_t> turnSamples_{0};    // samples fed to STT this turn (48 k-equivalent)
    PrerollRing*         preroll_{nullptr};   // VTS ring, mapped read-only (lazy)
    std::vector<float>   backfill_;           // pre-wake history to splice (guarded by pcmMtx_)
    std::atomic<bool>    backfillPending_{false};
    bool                 sttDone_{false};     // guarded by pcmMtx_
    std::string          sttResult_;          // guarded by pcmMtx_
    std::thread          sttThread_;

    std::atomic<bool>    playing_{false};
    std::vector<float>   ttsWav_;
    uint32_t             ttsRate_{22050};
    std::atomic<size_t>  ttsOffset_{0};
    std::atomic<bool>    drainPending_{false};

    std::string          apiKey_;
    std::vector<std::pair<std::string,std::string>> history_;

    int  vadSpeechMs_  = 0;
    int  vadSilenceMs_ = 0;
    bool vadHadSpeech_ = false;
    bool vadEndSent_   = false;

    std::atomic<bool>   stopping_{false};
    std::thread         pwThread_, vadThread_, monThread_;
    std::unique_ptr<pw::PwClient> pwClient_;
    std::unique_ptr<pw::PwStream> capStream_;
    std::unique_ptr<pw::PwStream> playStream_;
};

} // namespace hermes

int main() {
    hermes::LlmConnector cc;
    cc.ConnectMsg(hermes::ModuleId::LLM_CONNECTOR);
    cc.Start();
    for (;;) pause();
    cc.Stop();
    return 0;
}
