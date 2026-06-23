#include "hermes/common/Catalog.hpp"
#include "hermes/common/EventMap.hpp"
#include "hermes/common/ModuleId.hpp"
#include "hermes/common/MsgBus.hpp"
#include <unistd.h>

// CLOUD_CONNECTOR (ModuleId 5) — ON-TARGET proxy (SDS §16.1). A PipeWire client
// locally (receives ABOX clean mono; plays TTS into the graph) and a network
// client remotely (WebSocket/gRPC to remote STT/LLM/TTS). PipeWire never reaches
// the cloud — only this connector's socket does.
namespace hermes {
class CloudConnector : public MsgBus, public EventMap<CloudConnector> {
public:
    CloudConnector() {
        Add(_Cloud::cmd::OPEN_STREAM,   &CloudConnector::onOpen);
        Add(_Cloud::cmd::CLOSE_STREAM,  &CloudConnector::onClose);
        Add(_Cloud::cmd::UTTERANCE_END, &CloudConnector::onUttEnd);
        Add(_Cloud::cmd::ABORT,         &CloudConnector::onAbort);
    }
    int ProcessMsg(CMsg* m) override { return Execute(m->hdr.id, m); }
private:
    void onOpen(const CMsg*)   {}  // open net stream; subscribe to ABOX clean mono (PipeWire)
    void onClose(const CMsg*)  {}
    void onUttEnd(const CMsg*) {}  // commit utterance to LLM
    void onAbort(const CMsg*)  {}  // barge-in: cancel in-flight LLM/TTS
};
} // namespace hermes

int main() {
    hermes::CloudConnector cc;
    cc.ConnectMsg(hermes::ModuleId::CLOUD_CONNECTOR);
    for (;;) pause();
    return 0;
}
