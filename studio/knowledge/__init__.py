"""Stage 8 — Story knowledge: scenes (Layer 3) + addressees + character dossiers (Layer 2).

Feeds the interactive half (UC-8, end_to_end_pipeline.md §6.1): the device role-loads a
character's dossier and its position-scoped scene knowledge so the listener can talk *to* the
character. Same tiering philosophy as the rest of the studio:

  rules tier (default, zero-dep)  — only claims the text proves: scene segmentation by
      location cue + chapter boundary, vocative/two-party addressees, verbatim quotes,
      co-presence relationships, style statistics. Persona fields stay empty, flagged
      needs_review — the rules tier never invents personality.
  llm tier (`--knowledge llm`)    — writes the persona (role, traits, backstory, style,
      catchphrases) via the `claude` CLI, content-hash cached so rebuilds are deterministic
      after the first run. Falls back to rules with a printed note.
"""
from __future__ import annotations

import hashlib
import re
from collections import Counter

from studio.contract import Scene, Dossier, KnowledgeItem, NARRATOR, UNKNOWN

# Common story locations for the rules-tier scene splitter. A new scene starts when narration
# moves the action ("into the market", "to a cave"). Deliberately generic nouns — titles and
# proper places are the LLM tier's job.
_LOCATIONS = (
    "city", "market", "cave", "sea", "ocean", "palace", "castle", "forest", "wood", "garden",
    "house", "home", "village", "kingdom", "mountain", "ship", "tower", "river", "lake",
    "beach", "island", "desert", "school", "shop", "street", "bridge", "farm", "field",
)
_LOC_RE = re.compile(
    r"\b(?:in|into|inside|to|at|through|across|under)\s+(?:the|a|an)\s+(" +
    "|".join(_LOCATIONS) + r")\b", re.IGNORECASE)
_SENT_END = re.compile(r"(?<=[.!?])\s+")
_WORD = re.compile(r"[A-Za-z']+")


def _scene_id(chapter: int, order_start: int) -> str:
    return "sc_" + hashlib.sha256(f"{chapter}:{order_start}".encode()).hexdigest()[:8]


# ── Layer 3: scene segmentation (rules tier) ─────────────────────────────────

def build_scenes(utterances: list, characters: list) -> list:
    """Segment the utterance stream into scenes; stamp scene_id back onto each utterance.

    Break rules (cheapest-first, honest): chapter boundary always; a narration line that
    moves the action to a NEW location ("She ran off into the market") starts a scene.
    """
    # Known limit: single-token, case-sensitive alias matching — a character named a common
    # capitalized word ("Hope", "Will") would over-match as participant/addressee. Accepted
    # for the rules tier (needs_review gates it); the LLM/NER tiers disambiguate properly.
    alias_to_id = {}
    for c in characters:
        for a in set(c.aliases + [c.canonical_name]):
            for tok in a.split():
                alias_to_id[tok] = c.id

    scenes, cur = [], None
    for u in utterances:
        loc = None
        if u.speaker_id == NARRATOR:
            m = _LOC_RE.search(u.text)
            if m:
                loc = m.group(1).lower()
        new_chapter = cur is None or u.chapter != cur.chapter
        # a move is only a move if the current scene already HAS a location — otherwise the
        # first location cue merely names the scene we are already in
        moved = (loc is not None and cur is not None
                 and cur.location != "unknown" and loc != cur.location)
        if new_chapter or moved:
            if cur is not None:
                scenes.append(cur)
            cur = Scene(scene_id=_scene_id(u.chapter, u.order), chapter=u.chapter,
                        order_start=u.order, order_end=u.order,
                        location=loc or "unknown")
        elif loc and cur.location == "unknown":
            cur.location = loc                       # first location named inside the scene
        cur.order_end = u.order
        u.scene_id = cur.scene_id
        # participants: speakers + any registry name mentioned
        if u.speaker_id not in (NARRATOR, UNKNOWN) and u.speaker_id not in cur.participants:
            cur.participants.append(u.speaker_id)
        for w in _WORD.findall(u.text):
            cid = alias_to_id.get(w)
            if cid and cid not in cur.participants:
                cur.participants.append(cid)
    if cur is not None:
        scenes.append(cur)

    for s in scenes:
        s.max_order = s.order_end
    return scenes


def summarize_scenes(scenes: list, utterances: list) -> None:
    """Rules-tier summary: first narration sentence of the scene (verbatim, never invented)."""
    by_order = {u.order: u for u in utterances}
    for s in scenes:
        for o in range(s.order_start, s.order_end + 1):
            u = by_order.get(o)
            if u is not None and u.speaker_id == NARRATOR:
                s.summary = _SENT_END.split(u.text.strip())[0][:200]
                break


# ── addressee post-pass (rules tier) ─────────────────────────────────────────

def assign_addressees(utterances: list, characters: list, scenes: list) -> None:
    """Fill utterance.addressee_id for dialogue where the text/scene proves it.

    tier a — vocative: quote starts (or ends) with a registry name, "Genie, why…" → that char.
    tier b — two-party scene: exactly one OTHER speaking participant in the scene → them.
    Anything else stays None (never guessed): the dossier only claims proven interlocutors.
    """
    name_to_id = {}
    for c in characters:
        for a in set(c.aliases + [c.canonical_name]):
            name_to_id[a] = c.id
    # bucket speakers by scene RANGE (the scenes list is the source of truth) rather than
    # relying on u.scene_id having been stamped by a prior build_scenes call
    def scene_of(order: int):
        for s in scenes:
            if s.order_start <= order <= s.order_end:
                return s.scene_id
        return None
    speakers_by_scene = {}
    for u in utterances:
        if u.speaker_id not in (NARRATOR, UNKNOWN):
            speakers_by_scene.setdefault(scene_of(u.order), set()).add(u.speaker_id)

    voc_head = re.compile(r"^\s*([A-Z][a-z]+)\s*[,!]")
    voc_tail = re.compile(r",\s*([A-Z][a-z]+)\s*[.!?]?\s*$")
    for u in utterances:
        if u.speaker_id in (NARRATOR, UNKNOWN):
            continue
        for rx in (voc_head, voc_tail):
            m = rx.search(u.text)
            if m:
                cid = name_to_id.get(m.group(1))
                if cid and cid != u.speaker_id:
                    u.addressee_id = cid
                    break
        if u.addressee_id is None:
            others = speakers_by_scene.get(scene_of(u.order), set()) - {u.speaker_id}
            if len(others) == 1:
                u.addressee_id = next(iter(others))


# ── Layer 2: dossiers ────────────────────────────────────────────────────────

def build_dossiers(doc, characters: list, utterances: list, scenes: list,
                   knowledge: str = "rules") -> dict:
    """{character_id: Dossier}. Rules tier always runs (it is the evidence base);
    the LLM tier overlays persona fields on top of it."""
    by_id = {c.id: c for c in characters}
    dossiers = {c.id: _dossier_rules(c, characters, utterances, scenes) for c in characters}

    if knowledge == "llm":
        try:
            from studio.knowledge.llm import enrich_dossiers
            enrich_dossiers(doc, by_id, dossiers, utterances, scenes)
        except (RuntimeError, OSError) as e:
            # narrow on purpose: "CLI missing / environment" degrades to rules; a genuine
            # code defect (TypeError etc.) must surface, not masquerade as unavailability.
            # Per-character reply failures are already isolated inside enrich_dossiers.
            print(f"[knowledge] LLM tier unavailable ({e}); dossiers stay at rules tier")
    return dossiers


def _dossier_rules(c, characters: list, utterances: list, scenes: list) -> Dossier:
    d = Dossier(character_id=c.id)
    names = {x.id: x.canonical_name for x in characters}

    mine = [u for u in utterances if u.speaker_id == c.id]
    d.quotes = [u.text for u in mine[:5]]
    d.emotion_range = sorted({u.emotion.label for u in mine} - {"neutral"}) or ["neutral"]

    if mine:
        words = [len(_WORD.findall(u.text)) for u in mine]
        q = sum(u.text.strip().endswith("?") for u in mine)
        x = sum(u.text.strip().endswith("!") for u in mine)
        d.speaking_style = (f"{len(mine)} line(s), ~{sum(words) // len(words)} words/line"
                            + (f", {q} question(s)" if q else "")
                            + (f", {x} exclamation(s)" if x else ""))

    # relationships the text proves: spoke-with (addressee pairs) > appears-with (co-presence)
    spoke_with = Counter()
    for u in utterances:
        if u.speaker_id == c.id and u.addressee_id:
            spoke_with[u.addressee_id] += 1
        elif u.addressee_id == c.id and u.speaker_id not in (NARRATOR, UNKNOWN):
            spoke_with[u.speaker_id] += 1
    together = Counter()
    for s in scenes:
        if c.id in s.participants:
            for p in s.participants:
                if p != c.id:
                    together[p] += 1
    for cid, n in together.most_common():
        who = names.get(cid)
        if not who:
            continue
        if spoke_with[cid]:
            d.relationships[who] = f"speaks with ({spoke_with[cid]} exchange(s))"
        else:
            d.relationships[who] = f"appears together in {n} scene(s)"

    # Layer-3 → Layer-2 bridge: what this character witnessed, spoiler-gated per scene
    d.knowledge = [KnowledgeItem(fact=s.summary or f"a scene at the {s.location}",
                                 max_order=s.max_order)
                   for s in scenes if c.id in s.participants and (s.summary or s.location != "unknown")]
    return d
