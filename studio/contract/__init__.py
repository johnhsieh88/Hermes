"""The bundle contract — the ONE seam shared by the Studio (producer) and Hermes (consumer).

Everything in this module is the wire/format agreement described in
`docs/end_to_end_pipeline.md` §3. Hermes's device-side ingestion (Part I §16.5) must mirror
`audio_key()` exactly, or the two halves will disagree on what a cached clip is named.

Pure stdlib. No heavy deps.
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass, field, asdict
from typing import Optional

CONTRACT_VERSION = "0.2"          # 0.2: + scenes.json, utterance.addressee_id/scene_id,
                                  #      characters.okf dossier sections (persona/knowledge)
STUDIO_VERSION = "0.2.0"

NARRATOR = "NARRATOR"          # reserved speaker id for narration spans
UNKNOWN = "UNKNOWN"           # attribution failed → human-review candidate

# Fixed 7-state emotion vocabulary (Book-to-Voice PDF Stage 4). Replaces Hermes's free-text tone.
EMOTIONS = ("neutral", "joy", "sadness", "anger", "fear", "surprise", "tenderness")


def audio_key(text: str, voice_profile_id: str, emotion_label: str, engine_version: str) -> str:
    """Content-hash cache key for a rendered clip — the audio filename stem.

    `hash(text + voice + emotion + engine_version)`. Editing one line invalidates exactly one
    clip. **Hermes must compute this identically** to resolve segment_idx → audio/<key>.opus.
    """
    h = hashlib.sha256()
    for part in (text, voice_profile_id, emotion_label, engine_version):
        h.update(part.encode("utf-8"))
        h.update(b"\x1f")  # unit separator so fields can't be confused across a concatenation
    return h.hexdigest()


def slugify(s: str) -> str:
    keep = "".join(c.lower() if c.isalnum() else "-" for c in s).strip("-")
    while "--" in keep:
        keep = keep.replace("--", "-")
    return keep or "untitled"


@dataclass
class Emotion:
    label: str = "neutral"          # one of EMOTIONS
    intensity: float = 0.0          # 0..1, capped conservatively upstream


@dataclass
class Utterance:
    id: str                          # stable: sha256(book_id+chapter+order)
    chapter: int
    order: int                       # GLOBAL monotonic sequence — playback sorts by this
    text: str
    speaker_id: str                  # character id | NARRATOR | UNKNOWN
    emotion: Emotion
    confidence: float                # 0..1; < tau → human review before render
    tier: str = "narration"          # which attribution tier decided this (provenance)
    audio: Optional[str] = None      # "audio/<key>.opus" once Stage 6 renders it; None until then
    source_span: Optional[list] = None  # [start,end] char offsets in the chapter (optional)
    addressee_id: Optional[str] = None  # who a dialogue line is spoken TO (v0.2; nullable)
    scene_id: Optional[str] = None      # FK into scenes.json (v0.2; nullable)

    def to_json(self) -> dict:
        d = asdict(self)
        return d


@dataclass
class Character:
    id: str
    canonical_name: str
    aliases: list = field(default_factory=list)
    gender: str = "unknown"          # male | female | unknown (heuristic)
    mention_count: int = 0
    voice_profile_id: Optional[str] = None


@dataclass
class Scene:
    """Layer-3 story knowledge unit: a contiguous utterance range in one place/time.

    `max_order` is the spoiler gate: the device may reveal this scene's facts only when the
    listener's position pointer has passed it (role-load filters knowledge by max_order ≤ pos).
    """
    scene_id: str                    # "sc_" + sha256(chapter:order_start)[:8]
    chapter: int
    order_start: int                 # inclusive, global utterance order
    order_end: int                   # inclusive
    location: str = "unknown"
    participants: list = field(default_factory=list)   # character ids present in the scene
    summary: str = ""                # 1–2 sentences; rules tier = first narration sentence
    max_order: int = 0               # facts revealable once pos_ > this (== order_end)


@dataclass
class KnowledgeItem:
    fact: str
    max_order: int                   # spoiler gate, same semantics as Scene.max_order


@dataclass
class Dossier:
    """Layer-2 character card content → rendered into characters.okf/<id>.md.

    Rules tier fills only what the text proves (quotes, interlocutors, co-presence, style
    stats) and leaves persona fields empty with needs_review=True; the LLM tier writes the
    persona and is subject to the same human-review gate as attribution.
    """
    character_id: str
    role: str = ""                   # one line: who they are in this story
    traits: list = field(default_factory=list)
    relationships: dict = field(default_factory=dict)   # canonical_name -> relation phrase
    speaking_style: str = ""
    emotion_range: list = field(default_factory=list)
    catchphrases: list = field(default_factory=list)
    backstory: str = ""
    quotes: list = field(default_factory=list)          # verbatim lines from the book
    knowledge: list = field(default_factory=list)       # [KnowledgeItem]
    generated_by: str = "rules"      # rules | llm (provenance)
    needs_review: bool = True


@dataclass
class VoiceProfile:
    voice_profile_id: str
    base_voice: str                  # timbre pick from the casting library
    engine: str = "xtts-v2"          # target TTS engine (Stage 6)
    seed: int = 0                    # deterministic → stable voice across chapters
    pitch: float = 1.0
    rate: float = 1.0
    style: str = "neutral"
