#include "hermes/common/Catalog.hpp"
#include "hermes/common/EventMap.hpp"
#include "hermes/common/ModuleId.hpp"
#include "hermes/common/MsgBus.hpp"
#include <string>
#include <unistd.h>

// LLM_CONNECTOR (ModuleId 5) — ON-TARGET inference router (SDS §16.1). A PipeWire client
// locally (receives ABOX clean mono; plays TTS into the graph) that routes each utterance to
// EITHER a local on-device LLM (simple/low-latency) OR a remote cloud LLM (complex queries),
// fronting STT/LLM/TTS. PipeWire never reaches the network — only this connector's socket does.
namespace hermes {
class LlmConnector : public MsgBus, public EventMap<LlmConnector> {
public:
    LlmConnector() {
        Add(_Llm::cmd::OPEN_STREAM,   &LlmConnector::onOpen);
        Add(_Llm::cmd::CLOSE_STREAM,  &LlmConnector::onClose);
        Add(_Llm::cmd::UTTERANCE_END, &LlmConnector::onUttEnd);
        Add(_Llm::cmd::ABORT,         &LlmConnector::onAbort);
    }
    int ProcessMsg(CMsg* m) override { return Execute(m->hdr.id, m); }

private:
    // Where this turn's inference runs. Local = on-device LLM (low latency, offline-capable);
    // Cloud = remote LLM (bigger model, tools/fresh knowledge). Chosen per utterance.
    enum class Target { Local, Cloud };

    void onOpen(const CMsg*) {
        // TODO: open the local STT session + subscribe to ABOX clean mono (PipeWire); on the
        // first complex turn, also dial the cloud socket. transcript_ accumulates STT_FINAL.
        transcript_.clear();
    }
    void onClose(const CMsg*) { transcript_.clear(); }

    // UTTERANCE_END = user stopped talking → commit the accumulated transcript to an LLM.
    // This is where the local-vs-cloud routing decision is made.
    void onUttEnd(const CMsg*) {
        const Target tgt = route(transcript_);
        SendMsg(ModuleId::SUPERVISOR, _Llm::evt::LLM_BEGIN, PRIO_NORMAL);   // notify either way
        if (tgt == Target::Local) runLocal(transcript_);
        else                      runCloud(transcript_);
        transcript_.clear();
    }
    void onAbort(const CMsg*) {
        // barge-in: cancel in-flight inference + TTS (local OR cloud), then drop the turn.
        // TODO: stop local decode / close cloud stream; flush any queued TTS_CHUNK.
        transcript_.clear();
    }

    // ── Routing policy (heuristic stub) ──
    // Simple/short/offline turns stay on-device; long or "complex" ones go to the cloud.
    // TODO: replace the length heuristic with a real policy — intent class, tool/RAG need,
    // token estimate, network availability, battery/thermal budget.
    static Target route(const std::string& utterance) {
        constexpr size_t kLocalMaxChars = 120;          // short turns → on-device
        return utterance.size() <= kLocalMaxChars ? Target::Local : Target::Cloud;
    }

    void runLocal(const std::string& /*text*/) {
        // TODO: on-device LLM (e.g. llama.cpp / NPU) → stream tokens → local TTS → TTS_CHUNK.
    }
    void runCloud(const std::string& /*text*/) {
        // TODO: stream the utterance to the cloud LLM over the socket → relay TTS_CHUNK back.
    }

    std::string transcript_;   // STT final text accumulated for the current utterance
};
} // namespace hermes

int main() {
    hermes::LlmConnector cc;
    cc.ConnectMsg(hermes::ModuleId::LLM_CONNECTOR);
    for (;;) pause();
    return 0;
}
