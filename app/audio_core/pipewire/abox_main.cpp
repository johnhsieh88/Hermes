// hermes_abox — the AUDIO_CORE process (ModuleId 2). The live data plane is the C
// engine: one PipeWire filter whose on_process drives the Core-Proportional Buffer Pool
// (buffer_pipeline.c) over the C abox_node graph (src→aec→beamform→dmx→capgate). This file is the
// ONLY pw_* boundary (the bridge); the nodes never see PipeWire. The control module drives
// the engine's EngineMode atomically from SET_MODE (approach B, no re-routing).
#include "audio_core/abox/abox_nodes.h"
#include "audio_core/abox/nodes/node_common.h"
#include "hermes/common/Log.h"
#include "audio_core/abox/abox_vdma.h"
#include "audio_core/abox/buffer_pipeline.h"
#include "audio_core/abox/reference_manager.h"
#include "audio_core/pipewire/Pw.hpp"
#include "hermes/common/Catalog.hpp"
#include "hermes/common/EventMap.hpp"
#include "hermes/common/ModuleId.hpp"
#include "hermes/common/MsgBus.hpp"
#include "hermes/common/TtsRefRing.hpp"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace hermes {

// Control: drives the C engine's mode atomic from SET_MODE. The MsgBus recv thread runs
// this; the RT data-loop reads the mode per block (lock-free) inside process_tick.
class AudioCore : public MsgBus, public EventMap<AudioCore> {
public:
    AudioCore(hermes_buffered_pipeline* eng, abox_node* capgate)
        : eng_(eng), capgate_(capgate) {
        Add(_AudioCore::cmd::SET_MODE,       &AudioCore::onSetMode);
        Add(_AudioCore::cmd::SET_VOLUME,     &AudioCore::onSetVolume);
        Add(_AudioCore::cmd::RESET_PIPELINE, &AudioCore::onReset);
        Add(_AudioCore::cmd::START_CAPTURE,  &AudioCore::onStartCapture);
        Add(_AudioCore::cmd::STOP_CAPTURE,   &AudioCore::onStopCapture);
        // TODO: DUCK_PLAYBACK / ARM_BARGE_IN → finer controls (in_tts epic).
    }
    int ProcessMsg(CMsg* m) override { return Execute(m->hdr.id, m); }

private:
    void onSetMode(const CMsg* m) {
        if (m->pBody && m->hdr.length >= sizeof(int)) {
            const int v = *static_cast<const int*>(m->pBody);   // EngineMode == abox_mode values
            hermes_pipeline_set_mode(eng_, static_cast<abox_mode>(v));
            HM_LOG_INFO("SET_MODE → %d (mask latches next block)", v);
            // Ack with MODE_CHANGED (§12.2 0x28B): the mode machine's reply half. Runs on
            // the MsgBus recv thread (non-RT) — this is what un-strands SS_BARGE_DUCK
            // (Supervisor::onModeChanged is its only exit) once a BARGE_IN producer exists.
            SendMsg(ModuleId::SUPERVISOR, _AudioCore::evt::MODE_CHANGED, PRIO_NORMAL,
                    &v, sizeof v);
        }
    }
    void onSetVolume(const CMsg* m) {
        if (m->pBody && m->hdr.length >= sizeof(float))
            hermes_pipeline_set_gain(eng_, *static_cast<const float*>(m->pBody));  // 0..N, 1.0 = unity
    }
    void onReset(const CMsg*) {}                       // TODO: §6.2 reset pipeline
    // CAPGATE (§13.2.1): the data-plane capture gate — out_0 emits silence when closed.
    void onStartCapture(const CMsg*) { HM_LOG_INFO("START_CAPTURE → capgate open");
                                       abox_capgate_set_open(capgate_, 1); }
    void onStopCapture(const CMsg*)  { HM_LOG_INFO("STOP_CAPTURE → capgate closed");
                                       abox_capgate_set_open(capgate_, 0); }
    hermes_buffered_pipeline* eng_;
    abox_node*                capgate_;
};

} // namespace hermes

// Context bundled into the PipeWire callback user pointer.
struct AboxCtx {
    hermes_buffered_pipeline* eng    = nullptr;
    abox_ref_manager*         ref    = nullptr;
    hermes::TtsRefRing*       ttsref = nullptr;
    float ref_phase = 0.0f;   // cross-block phase carry for 22050→48000 SRC
    float ref_last  = 0.0f;   // last read sample — used to pad a starved ring

    // Ratio of input (22050 Hz) to output (48000 Hz) sample rates.
    static constexpr float kRefRatio = 22050.0f / 48000.0f;

    // The one pw_filter → C engine boundary. Per-quantum, RT thread.
    // Reads TTS reference from shm ring, resamples to 48 kHz, populates the AEC
    // ref manager, then hands control to the DSP pipeline.
    static void process(void* user, const float* const* in, int chIn,
                        float* const* out, int chOut, uint32_t n, uint64_t samplePos) {
        auto& self = *static_cast<AboxCtx*>(user);

        if (self.ttsref) {
            // For n output samples at 48 kHz, consume ≈ n * kRefRatio input samples
            // from the 22050 Hz ring. +2 gives one extra sample for interpolation plus
            // one guard against floating-point rounding.
            int needed = static_cast<int>(self.ref_phase + static_cast<float>(n) * kRefRatio) + 2;
            if (needed > 128) needed = 128;   // quantum is locked at 240; needed ≤ 113 normally

            float raw[128];
            int got = hermes::TtsRef_Read(self.ttsref, raw, needed);
            // Pad with the last known sample if the ring is temporarily starved
            // (e.g. TTS hasn't started yet). AEC will see a flat tail, not random noise.
            for (int i = got; i < needed; ++i) raw[i] = self.ref_last;
            if (got > 0) self.ref_last = raw[got - 1];

            // Linear interpolation: for each 48 kHz output index i, the fractional
            // position in the 22050 Hz input is ph = ref_phase + i * kRefRatio.
            float ref_out[256];   // 256 ≥ 240 (max quantum)
            float ph = self.ref_phase;
            for (uint32_t i = 0; i < n; ++i) {
                int   j  = static_cast<int>(ph);
                float fr = ph - static_cast<float>(j);
                float a  = (j   < needed) ? raw[j]   : self.ref_last;
                float b  = (j+1 < needed) ? raw[j+1] : self.ref_last;
                ref_out[i] = a + (b - a) * fr;
                ph += kRefRatio;
            }
            // Carry only the fractional part to the next block.
            float final_ph = self.ref_phase + static_cast<float>(n) * kRefRatio;
            self.ref_phase = final_ph - static_cast<float>(static_cast<int>(final_ph));

            abox_ref_write_farend(self.ref, ref_out, nullptr, static_cast<int>(n));
        }

        hermes_pipeline_process_tick(self.eng, in, chIn, out, chOut,
                                     static_cast<int>(n), samplePos);
    }
};

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
    abox_node* gate = abox_node_create("capgate");
    if (!src || !aec || !beam || !dmx || !gate) return 2;
    src->ops->prepare(src, &cfg);   aec->ops->prepare(aec, &cfg);
    beam->ops->prepare(beam, &cfg); dmx->ops->prepare(dmx, &cfg);
    gate->ops->prepare(gate, &cfg);
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
    hermes_pipeline_add_stage(&engine, gate, ABOX_ELEM_STRUCTURAL);  // capture gate (mode × cmd)

    // ── PipeWire: connect, control plane, then the one filter that runs the engine ──
    pw::PwClient client("hermes.abox");
    if (client.connect() != 0) return 1;

    AudioCore ctrl(&engine, gate);
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

    // Open the TTS reference ring (written by LLM_CONNECTOR s_playback). Both
    // processes use O_CREAT so startup order doesn't matter; fresh shm pages are
    // zero-initialised which matches the ring's idle wp=rp=0 state.
    static AboxCtx ctx;
    ctx.eng = &engine;
    ctx.ref = &ref;
    {
        int fd = shm_open("/hermes.ttsref", O_CREAT | O_RDWR, 0600);
        if (fd >= 0) {
            ftruncate(fd, sizeof(hermes::TtsRefRing));
            void* p = mmap(nullptr, sizeof(hermes::TtsRefRing),
                           PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            close(fd);
            ctx.ttsref = (p == MAP_FAILED) ? nullptr
                                           : static_cast<hermes::TtsRefRing*>(p);
        }
        if (!ctx.ttsref) fprintf(stderr, "[ABOX] ttsref shm unavailable — AEC ref disabled\n");
    }

    // ONE pw_filter: 2 mic-in, 1 mono-out; on_process walks the whole C graph (Model B).
    // VTS and LLM_CONNECTOR capture streams target "hermes.abox" so WirePlumber links
    // this filter's out_0 to their input ports. in_1 (TTS ref) is fed via the ttsref
    // shm ring rather than a PipeWire link to avoid WirePlumber routing rules.
    auto node = pw::create_pw_node(client, "hermes.abox", 2, 1,
                                   &AboxCtx::process, &ctx, 48000, 240);
    if (!node) { hermes_pipeline_stop_async(&engine); return 3; }

    client.run();   // PipeWire data-loop; the MsgBus recv thread drives the mode concurrently

    hermes_pipeline_stop_async(&engine);
    if (ctx.ttsref) munmap(ctx.ttsref, sizeof(hermes::TtsRefRing));
    abox_node_destroy(src);  abox_node_destroy(aec);
    abox_node_destroy(beam); abox_node_destroy(dmx);
    abox_node_destroy(gate);
    abox_node_destroy(vin);  abox_node_destroy(vout);
    return 0;
}
