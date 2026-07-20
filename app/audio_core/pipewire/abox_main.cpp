// hermes_abox — the AUDIO_CORE process (ModuleId 2). The live data plane is the C
// engine: one PipeWire filter whose on_process drives the Core-Proportional Buffer Pool
// (buffer_pipeline.c) over the C abox_node graph (src→aec→beamform→dmx). This file is the
// ONLY pw_* boundary (the bridge); the nodes never see PipeWire. The control module drives
// the engine's EngineMode atomically from SET_MODE (approach B, no re-routing).
#include "audio_core/abox/abox_nodes.h"
#include "audio_core/abox/abox_vdma.h"
#include "audio_core/abox/buffer_pipeline.h"
#include "audio_core/abox/reference_manager.h"
#include "audio_core/pipewire/Pw.hpp"
#include "hermes/common/Catalog.hpp"
#include "hermes/common/EventMap.hpp"
#include "hermes/common/ModuleId.hpp"
#include "hermes/common/MsgBus.hpp"
#include <cstdlib>
#include <cstring>

namespace hermes {

// Control: drives the C engine's mode atomic from SET_MODE. The MsgBus recv thread runs
// this; the RT data-loop reads the mode per block (lock-free) inside process_tick.
class AudioCore : public MsgBus, public EventMap<AudioCore> {
public:
    explicit AudioCore(hermes_buffered_pipeline* eng) : eng_(eng) {
        Add(_AudioCore::cmd::SET_MODE,       &AudioCore::onSetMode);
        Add(_AudioCore::cmd::SET_VOLUME,     &AudioCore::onSetVolume);
        Add(_AudioCore::cmd::RESET_PIPELINE, &AudioCore::onReset);
        // TODO: START_CAPTURE / DUCK_PLAYBACK / ARM_BARGE_IN → finer controls.
    }
    int ProcessMsg(CMsg* m) override { return Execute(m->hdr.id, m); }

private:
    void onSetMode(const CMsg* m) {
        if (m->pBody && m->hdr.length >= sizeof(int)) {
            const int v = *static_cast<const int*>(m->pBody);   // EngineMode == abox_mode values
            hermes_pipeline_set_mode(eng_, static_cast<abox_mode>(v));
        }
    }
    void onSetVolume(const CMsg* m) {
        if (m->pBody && m->hdr.length >= sizeof(float))
            hermes_pipeline_set_gain(eng_, *static_cast<const float*>(m->pBody));  // 0..N, 1.0 = unity
    }
    void onReset(const CMsg*) {}                       // TODO: §6.2 reset pipeline
    hermes_buffered_pipeline* eng_;
};

} // namespace hermes

// The per-quantum bridge (the one pw_* → C engine boundary). PipeWire hands mono channel
// pointers + frame count + sample timeline; the coordinator does the buffer-pool firewall,
// the mask-gated cascade, and the egress copy.
static void abox_block(void* user, const float* const* in, int chIn,
                       float* const* out, int chOut, uint32_t n, uint64_t samplePos) {
    auto* eng = static_cast<hermes_buffered_pipeline*>(user);
    hermes_pipeline_process_tick(eng, in, chIn, out, chOut, static_cast<int>(n), samplePos);
}

int main() {
    using namespace hermes;

    // ── Build the C engine: buffer pool + node graph + reference manager ──
    static hermes_buffered_pipeline engine;
    hermes_pipeline_init(&engine, 48000);

    static abox_ref_manager ref;
    abox_ref_prepare(&ref, 240);

    abox_config cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate = 48000;
    cfg.block_size  = 240;
    cfg.mic_channels = 2;

    abox_node* src  = abox_node_create("src");
    abox_node* aec  = abox_node_create("aec");
    abox_node* beam = abox_node_create("beamform");
    abox_node* dmx  = abox_node_create("dmx");
    if (!src || !aec || !beam || !dmx) return 2;
    src->ops->prepare(src, &cfg);   aec->ops->prepare(aec, &cfg);
    beam->ops->prepare(beam, &cfg); dmx->ops->prepare(dmx, &cfg);
    abox_aec_set_ref(aec, &ref);

    // Ingress/egress as real vDMA NODES (capture→slot / slot→playback). The coordinator
    // runs them on the RT thread, so they work with the async pool too.
    abox_node* vin  = abox_vdma_create(ABOX_VDMA_IN);
    abox_node* vout = abox_vdma_create(ABOX_VDMA_OUT);
    if (!vin || !vout) return 4;
    vin->ops->prepare(vin, &cfg);   vout->ops->prepare(vout, &cfg);
    hermes_pipeline_set_vdma(&engine, vin, vout);

    hermes_pipeline_add_stage(&engine, src,  ABOX_ELEM_SRC);
    hermes_pipeline_add_stage(&engine, aec,  ABOX_ELEM_AEC);
    hermes_pipeline_add_stage(&engine, beam, ABOX_ELEM_BEAM);
    hermes_pipeline_add_stage(&engine, dmx,  ABOX_ELEM_STRUCTURAL);  // always-on 2→1 (§13.2)

    // ── PipeWire: connect, control plane, then the one filter that runs the engine ──
    pw::PwClient client("hermes.abox");
    if (client.connect() != 0) return 1;

    AudioCore ctrl(&engine);
    ctrl.ConnectMsg(ModuleId::AUDIO_CORE);            // SET_MODE → engine mode

    // Engage the async buffer pool: one worker per slot on the RK3588 A76 big cores
    // (cpu5..; coordinator = the PipeWire RT thread). A slow block degrades to one
    // Soft-Mute period instead of stalling the loop. HERMES_SYNC=1 forces the inline path.
    if (!std::getenv("HERMES_SYNC"))
        hermes_pipeline_start_async(&engine, /*first_core=*/5);

    // Optional initial EngineMode for headless tests (0=KeywordListening idle/bypass-all,
    // 1=BargeInMuting, 2=Conversation full-duplex). Default stays KEYWORD_LISTENING.
    if (const char* ms = std::getenv("HERMES_MODE"))
        hermes_pipeline_set_mode(&engine, static_cast<abox_mode>(std::atoi(ms)));

    // ONE pw_filter: 2 mic-in, 1 mono-out; on_process walks the whole C graph (Model B).
    // Links (mic→hermes.abox→sink) are created by the init process / WirePlumber.
    auto node = pw::create_pw_node(client, "hermes.abox", 2, 1, &abox_block, &engine, 48000, 240);
    if (!node) { hermes_pipeline_stop_async(&engine); return 3; }

    client.run();   // PipeWire data-loop; the MsgBus recv thread drives the mode concurrently

    hermes_pipeline_stop_async(&engine);
    abox_node_destroy(src);  abox_node_destroy(aec);
    abox_node_destroy(beam); abox_node_destroy(dmx);
    abox_node_destroy(vin);  abox_node_destroy(vout);
    return 0;
}
