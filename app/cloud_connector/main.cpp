// CLOUD_CONNECTOR (ModuleId 5) — on-target AI proxy.
// PipeWire capture (mic) → amplitude VAD → STT (sherpa-onnx subprocess) →
// LLM (Groq via libcurl) → TTS (Piper subprocess) → PipeWire playback.
// Runs in its own process, communicates with Supervisor via MsgBus.
#include "audio_core/pipewire/Pw.hpp"
#include "hermes/common/Catalog.hpp"
#include "hermes/common/EventMap.hpp"
#include "hermes/common/ModuleId.hpp"
#include "hermes/common/MsgBus.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef HERMES_HAVE_CURL
#include <curl/curl.h>
#endif

namespace hermes {

// ── Constants ─────────────────────────────────────────────────────────────────
static constexpr uint32_t kCaptureRate  = 16000;
static constexpr float    kSilenceRMS   = 0.008f;  // -42 dBFS silence gate
static constexpr int      kSpeechMinMs  = 400;     // min speech before VAD fires
static constexpr int      kSilenceMs    = 900;     // silence window → endpoint
static constexpr int      kMaxUtterMs   = 20000;   // hard cap on utterance length

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
    static const char* kWav = "/tmp/cc-utterance.wav";
    write_wav(kWav, pcm, rate);
    char cmd[2048];
    snprintf(cmd, sizeof cmd,
        "sherpa-onnx-streaming-asr"
        " --encoder=%s/encoder-epoch-99-avg-1.int8.onnx"
        " --decoder=%s/decoder-epoch-99-avg-1.int8.onnx"
        " --joiner=%s/joiner-epoch-99-avg-1.int8.onnx"
        " --tokens=%s/tokens.txt"
        " %s 2>&1 | grep -o '\"text\":\"[^\"]*\"' | tail -1",
        kSttBase, kSttBase, kSttBase, kSttBase, kWav);
    FILE* p = popen(cmd, "r");
    if (!p) { unlink(kWav); return {}; }
    char buf[4096] = {};
    fread(buf, 1, sizeof buf - 1, p);
    pclose(p);
    unlink(kWav);
    // extract value from "text":"..."
    const char* needle = "\"text\":\"";
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
    std::vector<std::pair<std::string,std::string>>& history)
{
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

    // Extract "content":"..." from the choices array
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
    if (!reply.empty()) {
        history.push_back({"user", userText});
        history.push_back({"assistant", reply});
        if (history.size() > 20) history.erase(history.begin(), history.begin() + 2);
    }
    return reply;
}
#else
static std::string groq_chat(
    const std::string&, const std::string& userText,
    std::vector<std::pair<std::string,std::string>>&)
{
    fprintf(stderr, "[CC] libcurl not compiled in — echo: %s\n", userText.c_str());
    return "I heard you say: " + userText;
}
#endif

// ── TTS (Piper subprocess) ────────────────────────────────────────────────────
static WavPcm run_tts(const std::string& text) {
    static const char* kWav = "/tmp/cc-tts.wav";
    static const char* kTtsDir = "/opt/ensoul/models/tts";
    static const char* kVoice  = "en_US-hfc_female-medium";
    char model[512];
    snprintf(model, sizeof model, "%s/%s.onnx", kTtsDir, kVoice);
    // Piper reads from stdin
    FILE* p = popen(
        (std::string("piper --model ") + model +
         " --output_file " + kWav +
         " --sentence_silence 0.1 2>/dev/null").c_str(), "w");
    if (!p) return {};
    fwrite(text.c_str(), 1, text.size(), p);
    pclose(p);
    WavPcm wav = load_wav(kWav);
    unlink(kWav);
    return wav;
}

// ── CloudConnector ────────────────────────────────────────────────────────────
class CloudConnector : public MsgBus, public EventMap<CloudConnector> {
public:
    CloudConnector() {
        Add(_Cloud::cmd::OPEN_STREAM,   &CloudConnector::onOpen);
        Add(_Cloud::cmd::CLOSE_STREAM,  &CloudConnector::onClose);
        Add(_Cloud::cmd::UTTERANCE_END, &CloudConnector::onUttEnd);
        Add(_Cloud::cmd::ABORT,         &CloudConnector::onAbort);
        apiKey_ = read_key("GROQ_API_KEY");
    }

    int ProcessMsg(CMsg* m) override { return Execute(m->hdr.id, m); }

    void Start() {
        stopping_ = false;
        monThread_  = std::thread([this]{ monLoop();  });
        vadThread_  = std::thread([this]{ vadLoop();  });
        pwThread_   = std::thread([this]{ pwMain();   });
    }

    void Stop() {
        stopping_ = true;
        if (pwClient_) pwClient_->quit();
        if (pwThread_.joinable())  pwThread_.join();
        if (vadThread_.joinable()) vadThread_.join();
        if (monThread_.joinable()) monThread_.join();
    }

private:
    // ── MsgBus handlers (MsgBus recv thread) ──────────────────────────────────
    void onOpen(const CMsg*) {
        {
            std::lock_guard<std::mutex> lk(pcmMtx_);
            pcmBuf_.clear();
        }
        vadSpeechMs_  = 0;
        vadSilenceMs_ = 0;
        vadHadSpeech_ = false;
        vadEndSent_   = false;
        abort_        = false;
        capturing_    = true;
    }

    void onClose(const CMsg*)  { capturing_ = false; }

    void onUttEnd(const CMsg*) {
        capturing_ = false;
        std::thread([this]{ pipeline(); }).detach();
    }

    void onAbort(const CMsg*) {
        abort_    = true;
        capturing_ = false;
        playing_   = false;
    }

    // ── VAD thread: amplitude-based silence detection ─────────────────────────
    void vadLoop() {
        while (!stopping_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!capturing_ || vadEndSent_) continue;

            float rms = rms_.load(std::memory_order_relaxed);
            if (rms > kSilenceRMS) {
                vadSpeechMs_  += 50;
                vadSilenceMs_  = 0;
                if (vadSpeechMs_ >= kSpeechMinMs) vadHadSpeech_ = true;
            } else {
                vadSilenceMs_ += 50;
                if (vadHadSpeech_ && vadSilenceMs_ >= kSilenceMs) {
                    vadEndSent_ = true;
                    SendMsg(ModuleId::SUPERVISOR, _Cloud::evt::STT_ENDPOINT, PRIO_NORMAL);
                }
            }
            // Hard cap
            if (vadSpeechMs_ + vadSilenceMs_ >= kMaxUtterMs && !vadEndSent_) {
                vadEndSent_ = true;
                SendMsg(ModuleId::SUPERVISOR, _Cloud::evt::STT_ENDPOINT, PRIO_NORMAL);
            }
        }
    }

    // ── AI pipeline (detached thread per turn) ────────────────────────────────
    void pipeline() {
        // Copy captured audio under lock
        std::vector<int16_t> pcm;
        {
            std::lock_guard<std::mutex> lk(pcmMtx_);
            pcm = pcmBuf_;
        }
        if (pcm.empty() || abort_) { enterIdle(); return; }

        // STT
        std::string transcript = run_stt(pcm, kCaptureRate);
        fprintf(stderr, "[CC] transcript: %s\n", transcript.c_str());
        if (transcript.empty() || abort_) {
            SendMsg(ModuleId::SUPERVISOR, _Cloud::evt::STT_NO_SPEECH, PRIO_NORMAL);
            enterIdle(); return;
        }

        {
            uint32_t len = (uint32_t)transcript.size();
            SendMsg(ModuleId::SUPERVISOR, _Cloud::evt::STT_FINAL, PRIO_NORMAL,
                    transcript.c_str(), len);
        }
        if (abort_) { enterIdle(); return; }

        // LLM
        std::string reply = groq_chat(apiKey_, transcript, history_);
        fprintf(stderr, "[CC] reply: %s\n", reply.c_str());
        if (reply.empty() || abort_) {
            SendMsg(ModuleId::SUPERVISOR, _Cloud::evt::CLOUD_ERROR, PRIO_NORMAL);
            enterIdle(); return;
        }

        // TTS
        WavPcm wav = run_tts(reply);
        if (wav.f32.empty() || abort_) { enterIdle(); return; }

        // Stage playback (no mutex needed — playing_ is false, pipeline thread owns ttsWav_)
        ttsWav_   = std::move(wav.f32);
        ttsRate_  = wav.rate;
        ttsOffset_.store(0, std::memory_order_relaxed);

        // Signal Supervisor first chunk arrived → triggers PLAY_TTS + ARM_BARGE_IN
        SendMsg(ModuleId::SUPERVISOR, _Cloud::evt::TTS_CHUNK, PRIO_NORMAL);
        SendMsg(ModuleId::SUPERVISOR, _Cloud::evt::TTS_STREAM_END, PRIO_NORMAL);

        // Start PW playback — PW thread reads ttsWav_ via atomic offset
        playing_.store(true, std::memory_order_release);
    }

    void enterIdle() {
        abort_ = false;
    }

    // ── Monitor thread: detects playback drain, sends PLAYBACK_DRAINED ────────
    void monLoop() {
        while (!stopping_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (drainPending_.exchange(false)) {
                SendMsg(ModuleId::AUDIO_CORE, _AudioCore::evt::PLAYBACK_DRAINED, PRIO_NORMAL);
                playing_ = false;
            }
        }
    }

    // ── PipeWire thread ───────────────────────────────────────────────────────
    void pwMain() {
        using namespace pw;
        pwClient_ = std::make_unique<PwClient>("hermes-cloud-connector");
        if (pwClient_->connect() < 0) {
            fprintf(stderr, "[CC] PipeWire connect failed\n");
            return;
        }
        capStream_  = std::make_unique<PwStream>(*pwClient_, "hermes-cc-mic",
                                                 PwStream::CAPTURE, &CloudConnector::s_capture, this);
        playStream_ = std::make_unique<PwStream>(*pwClient_, "hermes-cc-tts",
                                                 PwStream::PLAYBACK, &CloudConnector::s_playback, this);
        capStream_->connect(kCaptureRate);
        playStream_->connect(22050);  // Piper hfc_female outputs 22050 Hz
        pwClient_->run();
    }

    // PW capture callback (RT thread)
    static void s_capture(void* u, float* samples, uint32_t nf, uint32_t /*rate*/) {
        auto& self = *static_cast<CloudConnector*>(u);
        // Compute RMS (lock-free)
        float rms = 0.0f;
        for (uint32_t i = 0; i < nf; ++i) rms += samples[i] * samples[i];
        self.rms_.store(std::sqrt(rms / (float)nf), std::memory_order_relaxed);

        if (!self.capturing_.load(std::memory_order_relaxed)) return;
        // Convert F32 → S16, append to buffer
        std::lock_guard<std::mutex> lk(self.pcmMtx_);
        for (uint32_t i = 0; i < nf; ++i) {
            float s = samples[i];
            if (s >  1.0f) s =  1.0f;
            if (s < -1.0f) s = -1.0f;
            self.pcmBuf_.push_back(static_cast<int16_t>(s * 32767.0f));
        }
    }

    // PW playback callback (RT thread)
    static void s_playback(void* u, float* samples, uint32_t nf, uint32_t /*rate*/) {
        auto& self = *static_cast<CloudConnector*>(u);
        if (!self.playing_.load(std::memory_order_acquire)) {
            std::memset(samples, 0, nf * sizeof(float));
            return;
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

    // ── State ──────────────────────────────────────────────────────────────────
    std::atomic<bool>     capturing_{false};
    std::atomic<bool>     abort_{false};
    std::atomic<float>    rms_{0.0f};
    std::mutex            pcmMtx_;
    std::vector<int16_t>  pcmBuf_;

    std::atomic<bool>     playing_{false};
    std::vector<float>    ttsWav_;
    uint32_t              ttsRate_{22050};
    std::atomic<size_t>   ttsOffset_{0};
    std::atomic<bool>     drainPending_{false};

    std::string           apiKey_;
    std::vector<std::pair<std::string,std::string>> history_;

    // VAD state (VAD thread only — no atomics needed)
    int  vadSpeechMs_  = 0;
    int  vadSilenceMs_ = 0;
    bool vadHadSpeech_ = false;
    bool vadEndSent_   = false;

    std::atomic<bool>   stopping_{false};
    std::thread         pwThread_;
    std::thread         vadThread_;
    std::thread         monThread_;
    std::unique_ptr<pw::PwClient> pwClient_;
    std::unique_ptr<pw::PwStream> capStream_;
    std::unique_ptr<pw::PwStream> playStream_;
};

} // namespace hermes

int main() {
    hermes::CloudConnector cc;
    cc.ConnectMsg(hermes::ModuleId::CLOUD_CONNECTOR);
    cc.Start();
    for (;;) pause();
    cc.Stop();
    return 0;
}
