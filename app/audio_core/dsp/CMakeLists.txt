# hermes_dsp — DSP nodes (SRC/AEC/Beamform/SES/ChannelMix, header-only) + factory.
# Pure C++, no PipeWire. NodeFactory.cpp is the one TU (also compiles every node).
add_library(hermes_dsp STATIC NodeFactory.cpp)
target_include_directories(hermes_dsp PUBLIC ${CMAKE_SOURCE_DIR}/app)
target_compile_features(hermes_dsp PUBLIC cxx_std_17)
