"""LLM tier for Stage 8 dossiers — persona authoring via the `claude` CLI.

Writes the fields the rules tier honestly can't (role, traits, backstory, catchphrases,
speaking_style prose, relationship phrasing) and refines scene knowledge facts. Everything the
LLM returns stays subordinate to the book text it is shown, and every card remains
needs_review=True — persona is product content and gets the same human gate as attribution.

Determinism: responses are cached under ~/.cache/hermes-studio/knowledge/ keyed by
sha256(the FULL prompt + prompt version) — the prompt embeds the book text, the rules-tier
evidence, and the scene list, so any change to upstream logic that alters what the model is
asked automatically misses the cache. A rebuild reuses the cached card byte-for-byte; only a
cache miss talks to the model.
"""
from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
from pathlib import Path

from studio.contract import Dossier, KnowledgeItem

PROMPT_VERSION = "dossier-v1"
_CACHE = Path.home() / ".cache" / "hermes-studio" / "knowledge"
_TIMEOUT = 180


def _book_text(doc) -> str:
    return "\n\n".join(p for ch in doc.chapters for p in ch.paragraphs)


def _prompt(doc, char, dossier, scenes) -> str:
    scene_lines = "\n".join(
        f"- scene {i} (orders {s.order_start}-{s.order_end}, location: {s.location}): {s.summary}"
        for i, s in enumerate(scenes))
    return f"""You are the character-dossier writer of an audiobook studio. Using ONLY the book
text below (plus, for famous public stories, widely known context that does not contradict it),
write the persona card for the character {char.canonical_name!r}.

BOOK TEXT:
{_book_text(doc)[:12000]}

SCENES (with global utterance-order ranges):
{scene_lines}

EVIDENCE ALREADY EXTRACTED (do not contradict it):
verbatim quotes: {dossier.quotes}
proven relationships: {dossier.relationships}

Return ONLY a JSON object, no prose, with exactly these keys:
{{"role": "one line: who they are in this story",
  "traits": ["3-6 adjectives"],
  "relationships": {{"OtherName": "relation phrase, e.g. 'his enemy', 'her father'"}},
  "speaking_style": "one line describing how they talk (for a voice actor)",
  "catchphrases": ["0-3 short phrases, only if the text/canon supports them"],
  "backstory": "2-3 sentences, child-appropriate, no events beyond this book's ending",
  "knowledge": [{{"fact": "one-sentence story fact this character witnessed",
                  "scene": <scene index from the list above>}}]}}"""


def extract_json(text: str) -> dict:
    """First parseable JSON object in `text`. A greedy `\\{.*\\}` regex breaks the moment the
    model echoes any braces before its answer (e.g. restating the evidence dict), so scan
    each '{' with raw_decode until one parses."""
    dec = json.JSONDecoder()
    start = text.find("{")
    while start != -1:
        try:
            obj, _ = dec.raw_decode(text, start)
            if isinstance(obj, dict):
                return obj
        except json.JSONDecodeError:
            pass
        start = text.find("{", start + 1)
    raise RuntimeError("no parseable JSON object in LLM reply")


def _ask_claude(prompt: str) -> dict:
    exe = shutil.which("claude")
    if not exe:
        raise RuntimeError("`claude` CLI not on PATH")
    r = subprocess.run([exe, "-p", "--output-format", "text", prompt],
                       capture_output=True, encoding="utf-8", timeout=_TIMEOUT)
    if r.returncode != 0:
        raise RuntimeError(f"claude CLI failed: {(r.stderr or '').strip()[:200]}")
    return extract_json(r.stdout)


def _str_list(v, cap: int) -> list:
    return [str(t)[:100] for t in v[:cap]] if isinstance(v, list) else []


def apply_llm_card(d: Dossier, data: dict, scenes: list) -> None:
    """Overlay one validated LLM reply onto a rules-tier dossier. Shape-checked field by
    field — the model does not always honor the requested schema, and a wrong type must
    degrade to 'field skipped', never crash the character (let alone the book)."""
    if isinstance(data.get("role"), str):
        d.role = data["role"][:200]
    d.traits = _str_list(data.get("traits"), 6) or d.traits
    if isinstance(data.get("speaking_style"), str):
        d.speaking_style = data["speaking_style"][:200]
    d.catchphrases = _str_list(data.get("catchphrases"), 3)
    if isinstance(data.get("backstory"), str):
        d.backstory = data["backstory"][:600]
    rel = data.get("relationships")
    if isinstance(rel, dict):
        for who, r in rel.items():
            d.relationships[str(who)[:60]] = str(r)[:100]
    llm_knowledge = []
    if isinstance(data.get("knowledge"), list):
        for item in data["knowledge"]:
            if not isinstance(item, dict):
                continue
            idx = item.get("scene")
            if isinstance(idx, int) and 0 <= idx < len(scenes):
                llm_knowledge.append(KnowledgeItem(fact=str(item.get("fact", ""))[:300],
                                                   max_order=scenes[idx].max_order))
    if llm_knowledge:
        d.knowledge = llm_knowledge                  # refined facts, same spoiler gates
    d.generated_by = "llm"
    d.needs_review = True                            # persona always passes the human gate


def enrich_dossiers(doc, by_id: dict, dossiers: dict, utterances: list, scenes: list) -> None:
    if not shutil.which("claude"):
        raise RuntimeError("`claude` CLI not on PATH")
    _CACHE.mkdir(parents=True, exist_ok=True)

    for cid, d in dossiers.items():
        char = by_id[cid]
        prompt = _prompt(doc, char, d, scenes)
        # key over the FULL prompt: book text, evidence, and scene list all shape the answer
        key = hashlib.sha256(f"{PROMPT_VERSION}\x1f{prompt}".encode()).hexdigest()[:16]
        cache_file = _CACHE / f"{key}.json"
        try:
            if cache_file.exists():
                data = json.loads(cache_file.read_text(encoding="utf-8"))
            else:
                print(f"   [knowledge] LLM dossier for {char.canonical_name} …")
                data = _ask_claude(prompt)
                tmp = cache_file.with_suffix(".json.tmp")
                tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2),
                               encoding="utf-8")
                tmp.replace(cache_file)
            apply_llm_card(d, data, scenes)
        except (RuntimeError, OSError, subprocess.TimeoutExpired,
                json.JSONDecodeError) as e:
            # isolate per character: one bad reply must not strip the rest of the cast
            print(f"   [knowledge] LLM dossier FAILED for {char.canonical_name}: {e} "
                  f"— keeping rules tier for this character")
