set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Use the Yocto aarch64 glibc as the compiler sysroot so libc/libstdc++ headers
# resolve to aarch64, not x86_64.
set(YS "$ENV{HOME}/ensoul-yocto-bsp/build/tmp/sysroots-components/cortexa57")
set(CMAKE_SYSROOT "${YS}/glibc")
set(CMAKE_C_FLAGS_INIT   "-mcpu=cortex-a57")
set(CMAKE_CXX_FLAGS_INIT "-mcpu=cortex-a57")

# PipeWire + SPA + CURL include paths (added per-target via PIPEWIRE_INCLUDE_DIRS etc.)
set(PW_INC   "${YS}/pipewire/usr/include/pipewire-0.3"
             "${YS}/pipewire/usr/include/spa-0.2")
set(GLIB_INC "${YS}/glib-2.0/usr/include/glib-2.0"
             "${YS}/glib-2.0/usr/lib/glib-2.0/include")
set(CURL_INC    "${YS}/curl/usr/include")
set(SHERPA_INC  "${YS}/sherpa-onnx/usr/include")

# Full-path .so references bypass sysroot library search entirely
set(PW_SO     "${YS}/pipewire/usr/lib/libpipewire-0.3.so")
set(CURL_SO   "${YS}/curl/usr/lib/libcurl.so")
set(SHERPA_SO "${YS}/sherpa-onnx/usr/lib/libsherpa-onnx-c-api.so")

# Pre-fill CMake find-package variables so no host packages are consulted.
# FindPkgConfig / FindCURL look at these before running their own probes.
set(PIPEWIRE_FOUND        TRUE CACHE BOOL "" FORCE)
set(PIPEWIRE_INCLUDE_DIRS "${PW_INC};${GLIB_INC}" CACHE STRING "" FORCE)
set(PIPEWIRE_LIBRARIES    "${PW_SO}" CACHE STRING "" FORCE)

# FindCURL looks for CURL_LIBRARY (singular) and CURL_INCLUDE_DIR (singular)
set(CURL_FOUND        TRUE CACHE BOOL "" FORCE)
set(CURL_LIBRARY      "${CURL_SO}" CACHE FILEPATH "" FORCE)
set(CURL_INCLUDE_DIR  "${CURL_INC}" CACHE PATH "" FORCE)
set(CURL_INCLUDE_DIRS "${CURL_INC}" CACHE STRING "" FORCE)
set(CURL_LIBRARIES    "${CURL_SO}" CACHE STRING "" FORCE)

# sherpa-onnx C API — find_library(SHERPA_C_LIB) + find_path(SHERPA_C_INC) in CMakeLists
set(SHERPA_C_LIB "${SHERPA_SO}"   CACHE FILEPATH "" FORCE)
set(SHERPA_C_INC "${SHERPA_INC}"  CACHE PATH     "" FORCE)

# Tell the cross-linker where to find shared-library dependencies of libcurl.so
# (openssl, zlib, libidn2, and pipewire itself) without embedding those paths.
# -rpath-link is a link-time hint only — the binary finds libs via ld.so at runtime.
set(_RL
    "-Wl,-rpath-link,${YS}/pipewire/usr/lib"
    "-Wl,-rpath-link,${YS}/openssl/usr/lib"
    "-Wl,-rpath-link,${YS}/zlib/usr/lib"
    "-Wl,-rpath-link,${YS}/libidn2/usr/lib"
    "-Wl,-rpath-link,${YS}/libunistring/usr/lib"
    "-Wl,-rpath-link,${YS}/sherpa-onnx/usr/lib"
    "-Wl,-rpath-link,${YS}/glibc/lib/aarch64-linux-gnu"
    "-Wl,-rpath-link,${YS}/glibc/usr/lib/aarch64-linux-gnu"
)
string(JOIN " " _RL_STR ${_RL})
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_RL_STR}")

set(CMAKE_FIND_ROOT_PATH              "${YS}/glibc")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
