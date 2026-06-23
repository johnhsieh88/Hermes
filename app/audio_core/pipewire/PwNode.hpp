#pragma once
#include "audio_core/dsp/Chain.hpp"
#include "audio_core/pipewire/ControlBridge.hpp"

// PipeWire SPA filter node hosting the DSP chain (SDS §14.4). Inputs: mic + far-end
// reference; output: clean mono. On each quantum it reads spa_io_position, fills an
// AudioBuffer from the input ports, runs the Chain, and writes the output port.
// Reads SharedControl atomics at the top of process() (§14.8/§14.9).
//
// Only built when PipeWire is found (HERMES_HAVE_PIPEWIRE); see app/CMakeLists.txt.
namespace hermes::pw {

class PwNode {
public:
    PwNode(dsp::Chain& chain, SharedControl& ctl);
    ~PwNode();
    PwNode(const PwNode&) = delete;
    PwNode& operator=(const PwNode&) = delete;

    int  init(int sampleRate, int quantum);   // pw_init, create filter + ports
    void run();                               // pw_main_loop_run (blocks)
    void quit();

private:
    struct Impl;
    Impl*          impl_;
    dsp::Chain&    chain_;
    SharedControl& ctl_;
};

} // namespace hermes::pw
