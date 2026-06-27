/* abox_atomic.h — atomic type compat so the C data-plane structs (worker_pool,
 * buffer_pipeline, param_store) are parseable from BOTH C and C++. `_Atomic` is a C11
 * keyword that C++ does not accept; `std::atomic<T>` is its ABI-identical C++ counterpart
 * (same size/alignment, lock-free for the scalar types used here). The .c files are
 * compiled as C and use the C11 `atomic_*` functions; a C++ TU (the PipeWire bridge) only
 * needs the struct layout to allocate/pass the engine, which this keeps identical. */
#ifndef HERMES_ABOX_ATOMIC_H
#define HERMES_ABOX_ATOMIC_H

#ifdef __cplusplus
  #include <atomic>
  #define ABOX_ATOMIC(T) std::atomic<T>
#else
  #include <stdatomic.h>
  #define ABOX_ATOMIC(T) _Atomic T
#endif

#endif /* HERMES_ABOX_ATOMIC_H */
