# Test samples

48 kHz / 16-bit PCM WAVs for validating the Hermes audio path (ALSA → `hermes.abox` →
speaker). Regenerate any time with:

```bash
python3 scripts/gen_samples.py     # stdlib only, no numpy
```

| File | What it is | Use it to check |
|------|------------|-----------------|
| `sine_440_mono.wav` | 2 s, 440 Hz tone | Basic playback — a clean A4 should come out the speaker |
| `sweep_100_8000.wav` | 3 s log-ish chirp 100→8000 Hz | Frequency response / aliasing through the SRC node |
| `stereo_LR_440_880.wav` | 2 s, L=440 Hz R=880 Hz | Channel mapping — left and right must not be swapped |
| `two_tone_440_660.wav` | 2 s, 440+660 Hz mix | Richer content for SRC / spectral inspection |
| `beeps_x3.wav` | 3 × 1 kHz beeps with gaps | Count-by-ear: confirms no dropped blocks / glitches |
| `noise_burst.wav` | 1.5 s deterministic noise | Broadband level metering; repeatable across runs |

## Playing one through the engine

The GUI (`scripts/run_gui.sh` → http://localhost:8080) lists these and plays them on
**Play**. Under the hood it runs PipeWire's `pw-play` targeting the engine node:

```bash
pw-play --target hermes.abox samples/sine_440_mono.wav
```

That feeds the WAV into `hermes.abox`, through the abox node graph (src→aec→beamform→ses),
and out to the default sink — the same path validated by `scripts/run_target.sh`.
