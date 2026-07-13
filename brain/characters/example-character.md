---
name: example-character
description: Per-character voice + emotion profile for expressive narration (example)
metadata:
  type: character
  book: example-book
---

<!-- One file per character. The book-prep pipeline generates these (LLM casting pass), then
     the narration director maps lines → this voice + the per-line emotion tag. -->

**Character:** Pip the Fox
**Role:** clever, kind-hearted trickster; the listener's favorite so far

**Voice:**
- voice_id: `tts:warm-young-male-02`   (id into the on-device / cloud TTS voice set)
- base_pitch: slightly high
- pace: quick, bouncy

**Default emotion:** playful
**Emotion range:** playful · curious · sneaky · worried (in danger) · proud
**Speaking style:** short bursts, lots of questions, the odd giggle

**Catchphrase / tells:** "Ooh, I've got a plan!"

<!-- At runtime, a line is rendered as: voice_id + (emotion tag from the text-annotation pass)
     + intensity. Emotion comes from the book-prep tagging, not invented per playback. -->
