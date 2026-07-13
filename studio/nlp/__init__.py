"""Stage 2 — Character extraction & coreference → character registry.

RULES tier (default, zero-dep): find capitalized name candidates, keep those that recur or appear
with a dialogue verb, merge simple aliases (honorific/surname), and guess gender from nearby
pronouns/honorifics. This is deliberately high-precision / modest-recall.

ML tier (optional): pass `nlp="spacy"` to use spaCy NER (`en_core_web_trf`) + coref for far better
recall. Guarded import so the rules tier always runs.
"""
from __future__ import annotations

import hashlib
import re
from collections import Counter, defaultdict

from studio.contract import Character
from studio.lexicon import (SAY_VERBS, PRONOUN_GENDER, MALE_HONORIFICS, FEMALE_HONORIFICS)

_HONOR = r"(?:Mr|Mrs|Ms|Miss|Dr|Sir|Lord|Lady|Master|Madam|Captain|Colonel|Professor)\.?"
_NAME = re.compile(rf"\b(?:{_HONOR}\s+)?([A-Z][a-z]+)\b")
_STOP = {  # common sentence-initial / non-name capitalized words to reject
    "The", "A", "An", "And", "But", "He", "She", "They", "It", "We", "You", "I", "His", "Her",
    "Their", "That", "This", "When", "Then", "At", "By", "In", "On", "Of", "As", "So", "For",
    "Chapter", "Book", "Part", "There", "Here", "Now", "Not", "No", "Yes", "If", "Some", "Someone",
}


def _char_id(name: str) -> str:
    return "ch_" + hashlib.sha256(name.encode()).hexdigest()[:8]


def _guess_gender(name: str, text: str, others: set) -> str:
    # the name itself is a gendered title used as a name ("the Sultan", "the Queen")
    if name.lower() in MALE_HONORIFICS:
        return "male"
    if name.lower() in FEMALE_HONORIFICS:
        return "female"
    # honorific in front of the name anywhere?
    for m in re.finditer(rf"\b({_HONOR})\s+{re.escape(name)}", text):
        h = m.group(1).lower().rstrip(".")
        if h in MALE_HONORIFICS:
            return "male"
        if h in FEMALE_HONORIFICS:
            return "female"
    # else: nearest gendered pronoun just after each mention — but STOP at another character's
    # name (a crude coref guard: don't let the next speaker's pronoun leak onto this name).
    # Proper coreference (fastcoref) is the optional ML tier; this is the cheap approximation.
    votes = Counter()
    toks = re.findall(r"[A-Za-z']+", text)
    for i, t in enumerate(toks):
        if t != name:
            continue
        for nxt in toks[i + 1:i + 7]:                 # short window
            if nxt in others:
                break                                 # another name → stop, don't cross
            g = PRONOUN_GENDER.get(nxt.lower())
            if g:
                votes[g] += 1
                break
    if votes:
        return votes.most_common(1)[0][0]
    return "unknown"


def build_registry_rules(doc, min_mentions: int = 2) -> list:
    full = "\n".join(p for ch in doc.chapters for p in ch.paragraphs)
    words = full.split()

    counts = Counter()
    after_of = Counter()                              # "city of Agrabah" → place, not a person
    near_say = set()                                  # names adjacent to a dialogue verb → strong
    for i, w in enumerate(words):
        m = _NAME.match(w)
        if not m:
            continue
        name = m.group(1)
        if name in _STOP:
            continue
        counts[name] += 1
        if i and words[i - 1].lower() in ("of", "in", "at", "from"):
            after_of[name] += 1
        ctx = " ".join(words[max(0, i - 2):i + 3]).lower()
        if any(v in ctx.split() for v in SAY_VERBS):
            near_say.add(name)

    # keep names that recur OR speak at least once; drop likely places (every mention is
    # "of/in/at/from X" and the name never speaks)
    keep = {n for n, c in counts.items()
            if (c >= min_mentions or n in near_say)
            and not (after_of[n] == c and n not in near_say)}

    # alias merge: a bare first name that is also the tail of a two-token name stays separate here;
    # simple case — merge names sharing a token is out of scope for the rules tier (documented).
    chars = []
    for name in sorted(keep, key=lambda n: (-counts[n], n)):
        c = Character(
            id=_char_id(name),
            canonical_name=name,
            aliases=[name],
            gender=_guess_gender(name, full, keep - {name}),
            mention_count=counts[name],
        )
        chars.append(c)
    return chars


def build_registry(doc, nlp: str = "rules", min_mentions: int = 2) -> list:
    if nlp == "spacy":
        try:
            return _build_registry_spacy(doc, min_mentions)
        except Exception as e:  # fall back, never fail the pipeline on an optional tier
            print(f"[nlp] spaCy tier unavailable ({e}); falling back to rules")
    return build_registry_rules(doc, min_mentions)


def _build_registry_spacy(doc, min_mentions: int):
    import spacy  # optional
    try:
        nlp = spacy.load("en_core_web_trf")
    except OSError:
        nlp = spacy.load("en_core_web_sm")            # smaller model fallback
    full = "\n".join(p for ch in doc.chapters for p in ch.paragraphs)
    counts = Counter()
    for ent in nlp(full).ents:
        if ent.label_ == "PERSON":
            counts[ent.text.split()[-1]] += 1         # last token ~ surname/first name
    chars = []
    for name, c in counts.items():
        if c < min_mentions:
            continue
        chars.append(Character(id=_char_id(name), canonical_name=name, aliases=[name],
                               gender=_guess_gender(name, full, set(counts) - {name}),
                               mention_count=c))
    # TODO: fastcoref/maverick-coref to fold pronouns + descriptors into these entities.
    return sorted(chars, key=lambda x: (-x.mention_count, x.canonical_name))
