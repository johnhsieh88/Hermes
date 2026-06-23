#pragma once
#include <cstdint>
#include <memory>

// Thin C++ wrapper over the PipeWire client API (SDS §14.3/§14.4). Forward-declared
// — NO pipewire headers here, so this is includable from any TU. The implementation
// (Pw.cpp) is the only file that includes <pipewire/pipewire.h> and is built only
// when libpipewire-0.3 is present.
//
// Mirrors the PipeWire object hierarchy:
//   PwClient   = pw_init + pw_main_loop + pw_context + pw_core   (the daemon connection)
//   PwFilterNode / create_pw_node = pw_filter + ports + connect  (forms a graph node)
struct pw_main_loop;
struct pw_context;
struct pw_core;
struct pw_filter;

namespace hermes::pw {

// RAII client connection. Ctor builds the loop + context; connect() opens the
// daemon socket and yields the core. (SDS "context created" path.)
class PwClient {
public:
    explicit PwClient(const char* appName = "hermes");
    ~PwClient();
    PwClient(const PwClient&) = delete;
    PwClient& operator=(const PwClient&) = delete;

    int   connect();                 // pw_context_connect → core (returns 0 on success)
    void  run();                     // pw_main_loop_run (blocks)
    void  quit();
    pw_main_loop* mainLoop() const;
    pw_core*      core() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Per-quantum DSP callback — PipeWire details (ports, spa_io_position) are hidden:
// you get mono channel pointers + frame count + the sample-position timeline.
using BlockFn = void (*)(void* user,
                         const float* const* in, int chIn,
                         float* const* out, int chOut,
                         uint32_t nframes, uint64_t samplePos);

// A pw_filter node with chIn + chOut mono DSP ports (mirrors pw_filter_new +
// add_port + connect). Ports only — links are made by the init process.
class PwFilterNode {
public:
    PwFilterNode(PwClient& client, const char* name, int chIn, int chOut, BlockFn fn, void* user);
    ~PwFilterNode();
    PwFilterNode(const PwFilterNode&) = delete;
    PwFilterNode& operator=(const PwFilterNode&) = delete;

    int        connect(int sampleRate, int quantum);   // node FORMED on the server here
    pw_filter* filter() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Function-form convenience: create + connect in one call (≈ a create_pw_node()).
std::unique_ptr<PwFilterNode> create_pw_node(PwClient& client, const char* name,
                                             int chIn, int chOut, BlockFn fn, void* user,
                                             int sampleRate, int quantum);

} // namespace hermes::pw
