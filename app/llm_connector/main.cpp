#include "hermes/common/Catalog.hpp"
#include "hermes/common/EventMap.hpp"
#include "hermes/common/ModuleId.hpp"
#include "hermes/common/MsgBus.hpp"
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
    void onOpen(const CMsg*)   {}  // open the stream; subscribe to ABOX clean mono (PipeWire)
    void onClose(const CMsg*)  {}
    void onUttEnd(const CMsg*) {}  // route utterance → local LLM (simple) or cloud LLM (complex)
    void onAbort(const CMsg*)  {}  // barge-in: cancel in-flight LLM/TTS (local or cloud)
};
} // namespace hermes

int main() {
    hermes::LlmConnector cc;
    cc.ConnectMsg(hermes::ModuleId::LLM_CONNECTOR);
    for (;;) pause();
    return 0;
}
