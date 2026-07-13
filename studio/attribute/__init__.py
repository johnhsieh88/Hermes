"""Stage 3 — Quotation detection & speaker attribution (THE CRUX).

Splits each paragraph into quoted-speech vs narration, then attributes every quote with a tiered,
cheapest-first cascade and a CONFIDENCE per attribution (PDF Stage 3):

  tier 1  explicit      "…," said Elias  /  Mara replied, "…"        conf 0.97
  tier 2  pronoun-gender "She laughed. '…'" + one female character   conf 0.70
  tier 3  nearest-name  closest registry name in the surrounding narration   conf 0.70
  tier 4  alternating   untagged back-and-forth between two speakers  conf 0.60
  tier 5  fallback      UNKNOWN → human-review candidate              conf 0.30

Narration → NARRATOR (conf 1.0). Everything below `tau` is what a review UI would surface before
any audio is rendered. The LLM sliding-window tier (PDF) plugs in as `attributor="llm"`.
"""
from __future__ import annotations

import hashlib
import re

from studio.contract import Utterance, Emotion, NARRATOR, UNKNOWN
from studio.lexicon import SAY_VERBS, PRONOUN_GENDER
from studio.emotion import tag_emotion

_QUOTE = re.compile(r'"([^"]*)"')
_WORD = re.compile(r"[A-Za-z']+")


def split_segments(paragraph: str) -> list:
    """Ordered [(kind, text)] where kind ∈ {'quote','narr'}."""
    segs, last = [], 0
    for m in _QUOTE.finditer(paragraph):
        if m.start() > last:
            n = paragraph[last:m.start()].strip()
            if n:
                segs.append(("narr", n))
        q = m.group(1).strip()
        if q:
            segs.append(("quote", q))
        last = m.end()
    tail = paragraph[last:].strip()
    if tail:
        segs.append(("narr", tail))
    return segs


def _uid(chapter: int, order: int, text: str) -> str:
    return "u_" + hashlib.sha256(f"{chapter}:{order}:{text}".encode()).hexdigest()[:12]


def _names_in(text: str, name_map: dict) -> list:
    """char_ids for capitalized tokens that match a registry alias, in order."""
    out = []
    for w in _WORD.findall(text):
        cid = name_map.get(w)
        if cid:
            out.append(cid)
    return out


def _has_say(text: str) -> bool:
    return any(w.lower() in SAY_VERBS for w in _WORD.findall(text))


def _attribute_quote(left, right, recent, name_map, gender_solo):
    head = " ".join(_WORD.findall(right)[:8])
    tail = " ".join(_WORD.findall(left)[-8:])

    # tier 1 — explicit: a say-verb AND a name close to the quote (either side)
    if _has_say(right):
        n = _names_in(head, name_map)
        if n:
            return n[0], 0.97, "explicit"
    if _has_say(tail):
        n = _names_in(tail, name_map)
        if n:
            return n[-1], 0.97, "explicit"

    # tier 2 — pronoun gender: a lone she/he near the quote + exactly one char of that gender
    ctx_words = [w.lower() for w in _WORD.findall(left + " " + right)]
    for w in ctx_words:
        g = PRONOUN_GENDER.get(w)
        if g and gender_solo.get(g):
            return gender_solo[g], 0.70, "pronoun-gender"

    # tier 3 — nearest name in the surrounding narration
    for side in (left, right):
        n = _names_in(side, name_map)
        if n:
            return n[0], 0.70, "nearest-name"

    # tier 4 — alternating turn between the two most recent distinct speakers
    distinct = []
    for s in reversed(recent):
        if s not in distinct:
            distinct.append(s)
        if len(distinct) == 2:
            break
    if len(distinct) == 2:
        return (distinct[1] if recent and recent[-1] == distinct[0] else distinct[0]), 0.60, "alternating"

    # tier 5 — give up; human review
    return UNKNOWN, 0.30, "fallback"


def attribute_doc(doc, characters, attributor: str = "rules") -> list:
    name_map = {}
    for c in characters:
        for a in c.aliases + [c.canonical_name]:
            for tok in a.split():
                name_map.setdefault(tok, c.id)
    # genders represented by exactly one character → usable for the pronoun tier
    by_gender = {}
    for c in characters:
        by_gender.setdefault(c.gender, []).append(c.id)
    gender_solo = {g: ids[0] for g, ids in by_gender.items() if len(ids) == 1 and g in ("male", "female")}

    if attributor == "llm":
        # Extension point: slide a 2–3k-token window with registry context, ask an LLM for
        # {quote_id, speaker_id, confidence} JSON, and override low-confidence rule results.
        print("[attribute] LLM tier not wired; using rules tier")

    utterances, order = [], 0
    for ch in doc.chapters:
        recent = []                                   # quote speakers within this chapter (scene run)
        for para in ch.paragraphs:
            segs = split_segments(para)
            for i, (kind, text) in enumerate(segs):
                if kind == "narr":
                    utterances.append(Utterance(
                        id=_uid(ch.index, order, text), chapter=ch.index, order=order,
                        text=text, speaker_id=NARRATOR, emotion=Emotion(), confidence=1.0,
                        tier="narration"))
                    order += 1
                    continue
                left = segs[i - 1][1] if i > 0 and segs[i - 1][0] == "narr" else ""
                right = segs[i + 1][1] if i + 1 < len(segs) and segs[i + 1][0] == "narr" else ""
                spk, conf, tier = _attribute_quote(left, right, recent, name_map, gender_solo)
                emo = tag_emotion(left, right)
                utterances.append(Utterance(
                    id=_uid(ch.index, order, text), chapter=ch.index, order=order,
                    text=text, speaker_id=spk, emotion=emo, confidence=conf, tier=tier))
                if spk not in (NARRATOR, UNKNOWN):
                    recent.append(spk)
                order += 1
    return utterances
