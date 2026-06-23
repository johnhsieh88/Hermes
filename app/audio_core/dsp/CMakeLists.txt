# hermes_dsp — the §1–§13 DSP island as a pure C++ library (no PipeWire).
# Linked into audio_core and into test/unit/dsp. Nodes are header-only; Chain.cpp
# is the one TU.
add_library(hermes_dsp STATIC Chain.cpp)
target_include_directories(hermes_dsp PUBLIC ${CMAKE_SOURCE_DIR}/app)
target_compile_features(hermes_dsp PUBLIC cxx_std_17)
