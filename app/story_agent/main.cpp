// STORY_AGENT (ModuleId 8) — multi-character audiobook orchestrator. Owns the active markdown
// script and the reading-position pointer, drives per-segment playback through LLM_CONNECTOR
// (which does the expressive per-character TTS), and on a user barge-in pauses, consults memory,
// and resumes. CONTROL PLANE ONLY — fully isolated from audio_core's RT/SPA threads; all
// long-latency work (memory recall, network) is kept off the IPC path (worker dispatch = TODO).
//
// Script format (one segment per line):  [Speaker|tone] dialogue text...
// Only the segment INDEX crosses the bus; speaker/tone/text stay local (see StoryMsg.hpp).
#include "hermes/common/Catalog.hpp"
#include "hermes/common/EventMap.hpp"
#include "hermes/common/ModuleId.hpp"
#include "hermes/common/MsgBus.hpp"
#include "hermes/common/StoryMsg.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace hermes {

struct Segment {
    std::string speaker;   // "Sherlock", "Narrator"
    std::string tone;      // "analytical", "calm"
    std::string text;      // dialogue (stays local; only the index is sent on the bus)
};

// Memory facade — the mem0 sidecar sits behind these three calls (recall/remember/export).
// Stubbed today; real calls dispatch to a worker thread so they never block the IPC loop.
class Memory {
public:
    std::string recall(const std::string& /*user*/, const std::string& /*query*/) {
        return "";   // TODO: mem0.search(user, query) → relevant facts
    }
    void remember(const std::string& /*user*/, const std::string& /*turn*/) {
        // TODO: mem0.add(user, turn) → ADD / UPDATE / DELETE conflict resolution
    }
    void exportMd(const std::string& /*user*/) {
        // TODO: render brain/memory/*.md for parent auditing
    }
};

class StoryAgent : public MsgBus, public EventMap<StoryAgent> {
public:
    StoryAgent() {
        Add(_Story::cmd::START,  &StoryAgent::onStart);
        Add(_Story::cmd::PAUSE,  &StoryAgent::onPause);
        Add(_Story::cmd::RESUME, &StoryAgent::onResume);
        // Driven by the connector: each finished segment advances the reading loop; STT during
        // playback is a barge-in. (Reuses existing _Llm events — no duplicate ids.)
        Add(_Llm::evt::TTS_STREAM_END, &StoryAgent::onSegmentDone);
        Add(_Llm::evt::STT_FINAL,      &StoryAgent::onUserSpeech);
    }
    int ProcessMsg(CMsg* m) override { return Execute(m->hdr.id, m); }

    int loadScript(const std::string& path) {
        std::ifstream f(path);
        if (!f) { std::fprintf(stderr, "story_agent: cannot open %s\n", path.c_str()); return -1; }
        segments_.clear();
        std::string line;
        while (std::getline(f, line)) {                       // "[Speaker|tone] text"; other lines skipped
            if (line.empty() || line[0] != '[') continue;
            const size_t bar = line.find('|'), end = line.find(']');
            if (bar == std::string::npos || end == std::string::npos || bar > end) continue;
            Segment s;
            s.speaker = line.substr(1, bar - 1);
            s.tone    = line.substr(bar + 1, end - bar - 1);
            const size_t t = line.find_first_not_of(' ', end + 1);
            s.text    = (t == std::string::npos) ? "" : line.substr(t);
            segments_.push_back(std::move(s));
        }
        std::printf("story_agent: loaded %zu segments from %s\n", segments_.size(), path.c_str());
        return static_cast<int>(segments_.size());
    }

private:
    enum class State { Idle, Reading, Paused, Interrupted };

    void play(int idx) {
        if (idx < 0 || idx >= static_cast<int>(segments_.size())) {
            SendMsg(ModuleId::SUPERVISOR, _Story::evt::STORY_DONE, PRIO_NORMAL);
            state_ = State::Idle;
            return;
        }
        const Segment& s = segments_[idx];
        std::printf("story_agent: ▶ seg %d  [%s|%s] %.48s\n",
                    idx, s.speaker.c_str(), s.tone.c_str(), s.text.c_str());
        StorySegmentRef ref{ idx };                            // 4-byte body — fits the lane
        SendMsg(ModuleId::LLM_CONNECTOR, _Llm::cmd::PLAY_SEGMENT, PRIO_NORMAL, &ref, sizeof(ref));
        SendMsg(ModuleId::SUPERVISOR, _Story::evt::SEGMENT_STARTED, PRIO_NORMAL, &ref, sizeof(ref));
    }

    void onStart(const CMsg*)  { pos_ = 0; state_ = State::Reading; play(pos_); }
    void onPause(const CMsg*)  { if (state_ == State::Reading) state_ = State::Paused; }
    void onResume(const CMsg*) { if (state_ == State::Paused) { state_ = State::Reading; play(pos_); } }

    void onSegmentDone(const CMsg*) {                          // connector finished a segment's audio
        if (state_ != State::Reading) return;
        play(++pos_);
    }

    void onUserSpeech(const CMsg* m) {                         // barge-in: pause → memory → resume
        if (state_ == State::Reading) state_ = State::Interrupted;
        std::string q;
        if (m && m->pBody && m->hdr.length) q.assign(static_cast<const char*>(m->pBody), m->hdr.length);
        (void)mem_.recall("listener", q);                     // TODO: feed facts into the answer prompt
        mem_.remember("listener", q);
        // (LLM_CONNECTOR generates/streams the spoken answer; we resume the story afterward.)
        if (state_ == State::Interrupted) { state_ = State::Reading; play(pos_); }
    }

    std::vector<Segment> segments_;
    int    pos_   = 0;
    State  state_ = State::Idle;
    Memory mem_;
};

} // namespace hermes

int main() {
    using namespace hermes;
    const char* path = std::getenv("HERMES_STORY");
    StoryAgent agent;
    agent.loadScript(path ? path : "data/stories/example.md");
    agent.ConnectMsg(ModuleId::STORY_AGENT);                  // own "/hermes.mod.8" inbox + recv thread
    std::printf("story_agent: up (ModuleId 8) — waiting for _Story::cmd::START\n");
    for (;;) pause();
    return 0;
}
