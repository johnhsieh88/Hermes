#!/usr/bin/env bash
# loopback.sh — REAL hermes_abox + PipeWire loopback, no audio hardware. Runs INSIDE the
# arm64 container (invoked by run_loopback.sh). Sets up two timer-driven PipeWire null sinks
# as virtual devices, then streams a sample through the live engine and captures the result:
#
#   pw-play → engine_in(null sink) ─monitor→ hermes.abox(DSP graph) → engine_out(null sink) → pw-record
#
# Output: out/loopback_out.wav (afplay it on macOS to HEAR the engine-processed audio).
#
# Two facts this works around:
#   1. WirePlumber needs a D-Bus session bus (else it crashes "without X11 $DISPLAY") — we
#      wrap the daemons in `dbus-run-session`.
#   2. hermes.abox is a non-autoconnect Filter; stream auto-routing won't link it. We create
#      persistent null sinks (stable ports) and pw-link the graph explicitly.
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends \
    pipewire wireplumber pipewire-bin python3 dbus dbus-bin >/dev/null 2>&1

export XDG_RUNTIME_DIR=/tmp/xdg
mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
mkdir -p /root/.config/pipewire/pipewire.conf.d /src/out

# Virtual devices, instantiated at daemon startup so they're persistent with stable ports.
# engine_in exposes monitor_FL/FR (what was played into it); engine_out is the capture point.
cat > /root/.config/pipewire/pipewire.conf.d/10-virt.conf <<'CONF'
context.objects = [
  { factory = adapter args = { factory.name = support.null-audio-sink node.name = engine_in  media.class = Audio/Sink audio.position = [ FL FR ] monitor.passthrough = true } }
  { factory = adapter args = { factory.name = support.null-audio-sink node.name = engine_out media.class = Audio/Sink audio.position = [ FL ] } }
]
CONF

# Everything that talks to PipeWire must share one D-Bus session (for WirePlumber).
exec dbus-run-session -- bash /src/scripts/_loopback_run.sh
