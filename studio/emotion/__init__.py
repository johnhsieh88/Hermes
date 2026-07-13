"""Stage 4 — Emotion / prosody tagging (cue-based rules tier).

Scans the narration around a quote for speech-verb / adverb cues and maps to the fixed 7-state
vocabulary + a conservative intensity. Neutral by default. The PDF's caution applies: cap intensity
low — "slightly-too-flat reads far better than caricature." A fine-tuned classifier or an LLM
per-chapter pass plugs in behind `tag_emotion` when higher fidelity is wanted.
"""
from __future__ import annotations

import re

from studio.contract import Emotion
from studio.lexicon import EMOTION_CUES

_WORD = re.compile(r"[A-Za-z]+")
_CAP = 0.6  # never exceed this from cues alone


def tag_emotion(left: str, right: str, classifier: str = "rules") -> Emotion:
    if classifier == "llm":
        # Extension point: batched LLM emotion pass per chapter, or a GoEmotions-derived classifier.
        pass
    best = Emotion()  # neutral, 0.0
    for w in _WORD.findall(f"{left} {right}".lower()):
        cue = EMOTION_CUES.get(w)
        if cue and cue[1] > best.intensity:
            best = Emotion(label=cue[0], intensity=min(cue[1], _CAP))
    return best
