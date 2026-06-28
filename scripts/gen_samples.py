#!/usr/bin/env python3
"""Generate dependency-free test WAV files for Hermes audio-path validation.

Writes 16-bit PCM WAVs into samples/ using only the stdlib (wave/struct/math), so it
runs anywhere with python3 — no numpy/scipy. Each clip targets a specific check
(channel mapping, AEC reference, SRC, level metering). See samples/README.md.
"""
import math
import struct
import wave
import os

SR = 48000  # match the Hermes engine sample rate (abox_config.sample_rate)
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "samples")


def _write(name, frames, channels):
    """frames: list of per-sample tuples (one value per channel), float in [-1, 1]."""
    path = os.path.join(OUT, name)
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)  # 16-bit
        w.setframerate(SR)
        buf = bytearray()
        for fr in frames:
            for s in fr:
                v = max(-1.0, min(1.0, s))
                buf += struct.pack("<h", int(v * 32767))
        w.writeframes(bytes(buf))
    print(f"  {name:24s} {len(frames)/SR:5.2f}s  {channels}ch")


def sine(freq, secs, amp=0.5, channels=1):
    n = int(SR * secs)
    return [tuple([amp * math.sin(2 * math.pi * freq * i / SR)] * channels) for i in range(n)]


def sweep(f0, f1, secs, amp=0.5):
    n = int(SR * secs)
    out = []
    for i in range(n):
        t = i / SR
        # linear chirp
        k = (f1 - f0) / secs
        phase = 2 * math.pi * (f0 * t + 0.5 * k * t * t)
        out.append((amp * math.sin(phase),))
    return out


def stereo_lr(secs, amp=0.5):
    """Left channel 440 Hz, right channel 880 Hz — verifies channel mapping isn't swapped."""
    n = int(SR * secs)
    return [(amp * math.sin(2 * math.pi * 440 * i / SR),
             amp * math.sin(2 * math.pi * 880 * i / SR)) for i in range(n)]


def two_tone(secs, amp=0.4):
    """440 + 660 Hz mixed — a richer signal for SRC / spectral checks."""
    n = int(SR * secs)
    return [(amp * (math.sin(2 * math.pi * 440 * i / SR) + math.sin(2 * math.pi * 660 * i / SR)) / 2,)
            for i in range(n)]


def beeps(count=3, secs_each=0.15, gap=0.25, freq=1000, amp=0.6):
    """Distinct beeps with silence gaps — easy to count by ear through the path."""
    out = []
    for _ in range(count):
        out += sine(freq, secs_each, amp)
        out += [(0.0,)] * int(SR * gap)
    return out


def noise_burst(secs, amp=0.3):
    """Deterministic pseudo-noise (LCG) — broadband content, repeatable across runs."""
    n = int(SR * secs)
    out = []
    state = 0x2545F4914F6CDD1D & 0xFFFFFFFFFFFFFFFF
    for _ in range(n):
        state = (6364136223846793005 * state + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        out.append((amp * ((state >> 40) / float(1 << 23) - 1.0),))
    return out


def main():
    os.makedirs(OUT, exist_ok=True)
    print(f">> writing test WAVs to {OUT}")
    _write("sine_440_mono.wav", sine(440, 2.0), 1)
    _write("sweep_100_8000.wav", sweep(100, 8000, 3.0), 1)
    _write("stereo_LR_440_880.wav", stereo_lr(2.0), 2)
    _write("two_tone_440_660.wav", two_tone(2.0), 1)
    _write("beeps_x3.wav", beeps(3), 1)
    _write("noise_burst.wav", noise_burst(1.5), 1)
    print(">> done.")


if __name__ == "__main__":
    main()
