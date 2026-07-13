// hermes_gui_interface — a DEV/TEST web bridge (ModuleId 7, SDS §14.5.1). It is NOT part of
// the on-device build: it exists so a human can drive the basic use cases (pick a sample,
// play it through hermes.abox, change engine mode, set volume, fire a barge-in, start/cancel
// a session) from a browser. Every UI action is translated into a control CMsg and put on the
// SAME POSIX-mq bus the real modules use (MsgBus::SendMsg) — so this is a faithful exercise of
// the control plane, not a side channel. Actual audio is fed into the engine via PipeWire's
// pw-play (the same path scripts/run_target.sh uses); the CMsg is the control signal alongside.
//
// Transport choice: a tiny dependency-free HTTP/1.1 server over POSIX sockets (no WebSocket
// handshake, no third-party libs) — button clicks don't need a streaming socket, and the
// event feed is a short poll. Single-threaded accept loop is plenty for one local operator.
#include "hermes/common/CMsg.hpp"
#include "hermes/common/Catalog.hpp"
#include "hermes/common/MsgBus.hpp"
#include "hermes/common/ModuleId.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace hermes {

// ── The embedded single-page UI (served at GET /). Inline so the binary needs no www/ dir. ──
static const char* kGuiHtml =
#include "gui_page.inc"
    ;

// id → short label, for the live event feed. Unknown ids show as "evt#<n>".
static const char* evt_name(uint16_t id) {
    using namespace _AudioCore::evt;
    switch (id) {
        case MODE_CHANGED:     return "AUDIO.MODE_CHANGED";
        case SOFT_MUTE:        return "AUDIO.SOFT_MUTE";
        case XRUN:             return "AUDIO.XRUN";
        case PLAYBACK_STARTED: return "AUDIO.PLAYBACK_STARTED";
        case PLAYBACK_DRAINED: return "AUDIO.PLAYBACK_DRAINED";
        case BARGE_IN:         return "AUDIO.BARGE_IN";
        case CAPTURE_STARTED:  return "AUDIO.CAPTURE_STARTED";
    }
    switch (id) {
        case _Supervisor::evt::STATE_CHANGED:   return "SUP.STATE_CHANGED";
        case _Supervisor::evt::SESSION_STARTED: return "SUP.SESSION_STARTED";
        case _Supervisor::evt::SESSION_ENDED:   return "SUP.SESSION_ENDED";
    }
    return nullptr;
}

// ── The bus bridge: subclass MsgBus so we own a real "/hermes.mod.7" inbox and can SendMsg
//    to any module. Inbound events are stashed in a small ring the UI polls via GET /api/events.
class GuiBridge : public MsgBus {
public:
    int ProcessMsg(CMsg* m) override {
        const char* n = evt_name(m->hdr.id);
        char line[96];
        if (n) std::snprintf(line, sizeof(line), "%s (from mod%u)", n, m->hdr.src);
        else   std::snprintf(line, sizeof(line), "evt#%u (from mod%u)", m->hdr.id, m->hdr.src);
        pushEvent(line);
        return 0;
    }

    void pushEvent(const std::string& s) {
        std::lock_guard<std::mutex> lk(mu_);
        events_.push_back(s);
        while (events_.size() > 64) events_.pop_front();
    }

    std::string eventsJson() {
        std::lock_guard<std::mutex> lk(mu_);
        std::string j = "[";
        for (size_t i = 0; i < events_.size(); ++i) {
            if (i) j += ",";
            j += "\"" + jsonEscape(events_[i]) + "\"";
        }
        j += "]";
        return j;
    }

    // ── Real audio: play a sample WAV (one child at a time). ──
    // Linux/device: pw-play --target hermes.abox → THROUGH the engine to the speaker.
    // macOS host:   afplay → straight to CoreAudio (engine is Linux-only, so raw file).
    // Override either with env HERMES_PLAYER=<binary> (invoked as `<binary> <path>`).
    void startPlay(const std::string& path) {
        stopPlay();
        pid_t pid = fork();
        if (pid == 0) {  // child
            if (const char* p = std::getenv("HERMES_PLAYER"))
                execlp(p, p, path.c_str(), static_cast<char*>(nullptr));
#if defined(__APPLE__)
            execlp("afplay", "afplay", path.c_str(), static_cast<char*>(nullptr));
#else
            execlp("pw-play", "pw-play", "--target", "hermes.abox", path.c_str(),
                   static_cast<char*>(nullptr));
#endif
            _exit(127);  // player not on PATH — harmless
        }
        if (pid > 0) playPid_.store(pid);
    }
    void stopPlay() {
        reap();
        pid_t p = playPid_.exchange(-1);
        if (p > 0) { kill(p, SIGTERM); int st = 0; waitpid(p, &st, 0); }
    }
    void reap() {  // collect a clip that finished on its own, so it doesn't linger as a zombie
        pid_t p = playPid_.load();
        if (p > 0) { int st = 0; if (waitpid(p, &st, WNOHANG) == p) playPid_.store(-1); }
    }

    static std::string jsonEscape(const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (c == '\n') o += "\\n";
            else o += c;
        }
        return o;
    }

private:
    std::mutex mu_;
    std::deque<std::string> events_;
    std::atomic<pid_t> playPid_{-1};
};

// ── Minimal JSON field extraction (control bodies are tiny and we generate the client). ──
static bool json_str(const std::string& body, const char* key, std::string& out) {
    std::string pat = "\"" + std::string(key) + "\"";
    size_t k = body.find(pat);
    if (k == std::string::npos) return false;
    size_t c = body.find(':', k + pat.size());
    if (c == std::string::npos) return false;
    size_t q1 = body.find('"', c + 1);
    if (q1 == std::string::npos) return false;
    size_t q2 = body.find('"', q1 + 1);
    if (q2 == std::string::npos) return false;
    out = body.substr(q1 + 1, q2 - q1 - 1);
    return true;
}
static bool json_num(const std::string& body, const char* key, double& out) {
    std::string pat = "\"" + std::string(key) + "\"";
    size_t k = body.find(pat);
    if (k == std::string::npos) return false;
    size_t c = body.find(':', k + pat.size());
    if (c == std::string::npos) return false;
    out = std::strtod(body.c_str() + c + 1, nullptr);
    return true;
}

// ── samples dir → JSON array of *.wav names (sorted). ──
static std::string samples_json(const std::string& dir) {
    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".wav") == 0) names.push_back(n);
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    std::string j = "[";
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) j += ",";
        j += "\"" + GuiBridge::jsonEscape(names[i]) + "\"";
    }
    j += "]";
    return j;
}

// ── Dispatch one UI action → control CMsg(s) on the bus. Returns a JSON ack. ──
static std::string dispatch(GuiBridge& bus, const std::string& dir, const std::string& body) {
    std::string action;
    if (!json_str(body, "action", action)) return "{\"ok\":false,\"err\":\"no action\"}";
    char msg[160] = {0};
    int rc = 0;

    if (action == "mode") {
        double v = 0; json_num(body, "value", v);
        int mode = static_cast<int>(v);
        rc = bus.SendMsg(ModuleId::AUDIO_CORE, _AudioCore::cmd::SET_MODE, PRIO_NORMAL, &mode, sizeof(mode));
        std::snprintf(msg, sizeof(msg), "SET_MODE=%d → AUDIO_CORE", mode);
    } else if (action == "volume") {
        double pct = 100; json_num(body, "value", pct);
        float gain = static_cast<float>(pct) / 100.0f;  // slider 0..150% → gain 0..1.5
        rc = bus.SendMsg(ModuleId::AUDIO_CORE, _AudioCore::cmd::SET_VOLUME, PRIO_NORMAL, &gain, sizeof(gain));
        std::snprintf(msg, sizeof(msg), "SET_VOLUME=%.2f → AUDIO_CORE", gain);
    } else if (action == "bargein") {
        int mode = 1;  // ABOX_MODE_BARGE_IN_MUTING — duck/mute playback while user talks
        bus.SendMsg(ModuleId::AUDIO_CORE, _AudioCore::cmd::DUCK_PLAYBACK, PRIO_URGENT, nullptr, 0);
        rc = bus.SendMsg(ModuleId::AUDIO_CORE, _AudioCore::cmd::SET_MODE, PRIO_URGENT, &mode, sizeof(mode));
        std::snprintf(msg, sizeof(msg), "BARGE-IN (DUCK_PLAYBACK + SET_MODE=1) → AUDIO_CORE");
    } else if (action == "play") {
        std::string file;
        if (!json_str(body, "file", file) || file.find("..") != std::string::npos)
            return "{\"ok\":false,\"err\":\"bad file\"}";
        bus.startPlay(dir + "/" + file);
        rc = bus.SendMsg(ModuleId::AUDIO_CORE, _AudioCore::cmd::START_CAPTURE, PRIO_NORMAL, nullptr, 0);
        std::snprintf(msg, sizeof(msg), "PLAY %s (pw-play→hermes.abox) + START_CAPTURE", file.c_str());
    } else if (action == "stop") {
        bus.stopPlay();
        rc = bus.SendMsg(ModuleId::AUDIO_CORE, _AudioCore::cmd::STOP_CAPTURE, PRIO_NORMAL, nullptr, 0);
        std::snprintf(msg, sizeof(msg), "STOP (kill pw-play) + STOP_CAPTURE");
    } else if (action == "session_start") {
        rc = bus.SendMsg(ModuleId::SUPERVISOR, _Supervisor::cmd::START_SESSION, PRIO_NORMAL, nullptr, 0);
        std::snprintf(msg, sizeof(msg), "START_SESSION → SUPERVISOR");
    } else if (action == "session_cancel") {
        rc = bus.SendMsg(ModuleId::SUPERVISOR, _Supervisor::cmd::CANCEL_SESSION, PRIO_NORMAL, nullptr, 0);
        std::snprintf(msg, sizeof(msg), "CANCEL_SESSION → SUPERVISOR");
    } else if (action == "reset") {
        rc = bus.SendMsg(ModuleId::AUDIO_CORE, _AudioCore::cmd::RESET_PIPELINE, PRIO_NORMAL, nullptr, 0);
        std::snprintf(msg, sizeof(msg), "RESET_PIPELINE → AUDIO_CORE");
    } else {
        return "{\"ok\":false,\"err\":\"unknown action\"}";
    }

    // rc<0 just means the target module isn't running — report it; the bridge stays up.
    char note[224];
    std::snprintf(note, sizeof(note), "%s  [%s]", msg, rc == 0 ? "sent" : "no peer");
    bus.pushEvent(std::string("⇢ ") + note);
    char resp[300];
    std::snprintf(resp, sizeof(resp), "{\"ok\":%s,\"msg\":\"%s\"}",
                  rc == 0 ? "true" : "false", GuiBridge::jsonEscape(note).c_str());
    return resp;
}

// ── Real STT via sherpa-onnx subprocess ──────────────────────────────────────
static const char* kSttBase =
    "/opt/ensoul/models/stt/sherpa-onnx-streaming-zipformer-en-20M-2023-02-17";

static std::string run_stt_on_wav(const char* wav_path) {
    char cmd[1024];
    // sherpa-onnx writes the JSON result line to stderr; redirect 2>&1 >/dev/null
    std::snprintf(cmd, sizeof(cmd),
        "sherpa-onnx"
        " --tokens=%s/tokens.txt"
        " --encoder=%s/encoder-epoch-99-avg-1.int8.onnx"
        " --decoder=%s/decoder-epoch-99-avg-1.int8.onnx"
        " --joiner=%s/joiner-epoch-99-avg-1.int8.onnx"
        " --model-type=zipformer --num-threads=4 %s 2>&1 >/dev/null",
        kSttBase, kSttBase, kSttBase, kSttBase, wav_path);
    FILE* fp = popen(cmd, "r");
    if (!fp) return "";
    std::string out;
    char buf[512];
    while (fgets(buf, sizeof(buf), fp)) out += buf;
    pclose(fp);
    // find: { "text": "hello world", ...}
    const std::string kNeedle = "\"text\": \"";
    size_t p = out.find(kNeedle);
    if (p == std::string::npos) return "";
    p += kNeedle.size();
    size_t q = out.find('"', p);
    if (q == std::string::npos) return "";
    return out.substr(p, q - p);
}

// ── tiny HTTP plumbing ──
static void send_resp(int fd, const char* status, const char* ctype, const std::string& body) {
    char hdr[256];
    int n = std::snprintf(hdr, sizeof(hdr),
                          "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                          "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                          status, ctype, body.size());
    if (write(fd, hdr, n) < 0) return;
    if (!body.empty()) (void)!write(fd, body.data(), body.size());
}

static int serve(GuiBridge& bus, const std::string& samplesDir, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(s, 16) < 0) { perror("listen"); return 1; }
    std::printf(">> hermes_gui_interface on http://localhost:%d  (samples: %s)\n", port,
                samplesDir.c_str());
    std::fflush(stdout);
    signal(SIGPIPE, SIG_IGN);

    for (;;) {
        int c = accept(s, nullptr, nullptr);
        if (c < 0) continue;
        bus.reap();  // opportunistically collect a finished pw-play

        // Read headers (+ body if Content-Length). STT posts can be several MB.
        std::string req;
        char buf[16384];
        ssize_t r;
        size_t header_end = std::string::npos;
        while ((header_end = req.find("\r\n\r\n")) == std::string::npos &&
               (r = read(c, buf, sizeof(buf))) > 0) {
            req.append(buf, static_cast<size_t>(r));
            if (req.size() > (1 << 20)) break;  // 1 MB guard
        }
        std::string body;
        if (header_end != std::string::npos) {
            size_t cl = 0, p = req.find("Content-Length:");
            if (p != std::string::npos) cl = std::strtoul(req.c_str() + p + 15, nullptr, 10);
            body = req.substr(header_end + 4);
            while (body.size() < cl && (r = read(c, buf, sizeof(buf))) > 0)
                body.append(buf, static_cast<size_t>(r));
        }

        std::string method = req.substr(0, req.find(' '));
        size_t ps = req.find(' ') + 1;
        std::string path = req.substr(ps, req.find(' ', ps) - ps);

        if (method == "GET" && (path == "/" || path == "/index.html")) {
            send_resp(c, "200 OK", "text/html; charset=utf-8", kGuiHtml);
        } else if (method == "GET" && path == "/api/samples") {
            send_resp(c, "200 OK", "application/json", samples_json(samplesDir));
        } else if (method == "GET" && path == "/api/events") {
            send_resp(c, "200 OK", "application/json", bus.eventsJson());
        } else if (method == "POST" && path == "/api/cmd") {
            send_resp(c, "200 OK", "application/json", dispatch(bus, samplesDir, body));
        } else if (method == "POST" && path == "/api/stt") {
            if (body.size() < 44 || body.size() > 10 * 1024 * 1024) {
                send_resp(c, "400 Bad Request", "application/json",
                          "{\"ok\":false,\"err\":\"body must be 44B–10MB WAV\"}");
            } else {
                const char* wav = "/tmp/hermes_stt_input.wav";
                if (FILE* f = fopen(wav, "wb")) { fwrite(body.data(), 1, body.size(), f); fclose(f); }
                char note[128];
                std::snprintf(note, sizeof(note), "STT: running sherpa-onnx on %.1f s WAV…",
                              static_cast<double>(body.size() - 44) / (2.0 * 16000));
                bus.pushEvent(std::string("⇢ ") + note);
                std::string transcript = run_stt_on_wav(wav);
                if (transcript.empty()) transcript = "(no speech detected)";
                bus.pushEvent("⇠ STT result: '" + transcript + "'");
                std::string resp = "{\"ok\":true,\"transcript\":\""
                                   + GuiBridge::jsonEscape(transcript) + "\"}";
                send_resp(c, "200 OK", "application/json", resp);
            }
        } else {
            send_resp(c, "404 Not Found", "text/plain", "not found");
        }
        close(c);
    }
}

} // namespace hermes

int main() {
    using namespace hermes;
    const char* p = std::getenv("HERMES_GUI_PORT");
    const char* d = std::getenv("HERMES_SAMPLES_DIR");
    int port = p ? std::atoi(p) : 8080;
    std::string dir = d ? d : "samples";

    GuiBridge bus;
    bus.ConnectMsg(ModuleId::GUI_INTERFACE);  // own "/hermes.mod.7" inbox + recv thread
    bus.pushEvent("gui_interface up — control plane bridge ready");
    return serve(bus, dir, port);
}
