"""LLM tier for Stage 8 dossiers — persona authoring via the `claude` CLI.

Writes the fields the rules tier honestly can't (role, traits, backstory, catchphrases,
speaking_style prose, relationship phrasing) and refines scene knowledge facts. Everything the
LLM returns stays subordinate to the book text it is shown, and every card remains
needs_review=True — persona is product content and gets the same human gate as attribution.

Determinism: responses are cached under ~/.cache/hermes-studio/knowledge/ keyed by
sha256(book text + character id + prompt version). A rebuild reuses the cached card
byte-for-byte; only a cache miss talks to the model.
"""
from __future__ import annotations

import hashlib
import json
import re
import shutil
import subprocess
from pathlib import Path

from studio.contract import KnowledgeItem

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


def _ask_claude(prompt: str) -> dict:
    exe = shutil.which("claude")
    if not exe:
        raise RuntimeError("`claude` CLI not on PATH")
    r = subprocess.run([exe, "-p", "--output-format", "text", prompt],
                       capture_output=True, encoding="utf-8", timeout=_TIMEOUT)
    if r.returncode != 0:
        raise RuntimeError(f"claude CLI failed: {(r.stderr or '').strip()[:200]}")
    m = re.search(r"\{.*\}", r.stdout, re.DOTALL)   # tolerate prose around the JSON
    if not m:
        raise RuntimeError("no JSON object in LLM reply")
    return json.loads(m.group(0))


def enrich_dossiers(doc, by_id, dossiers, utterances, scenes) -> None:
    _CACHE.mkdir(parents=True, exist_ok=True)
    book_sha = hashlib.sha256(_book_text(doc).encode()).hexdigest()[:12]

    for cid, d in dossiers.items():
        char = by_id[cid]
        key = hashlib.sha256(f"{book_sha}:{cid}:{PROMPT_VERSION}".encode()).hexdigest()[:16]
        cache_file = _CACHE / f"{key}.json"
        if cache_file.exists():
            data = json.loads(cache_file.read_text(encoding="utf-8"))
        else:
            print(f"   [knowledge] LLM dossier for {char.canonical_name} …")
            data = _ask_claude(_prompt(doc, char, d, scenes))
            tmp = cache_file.with_suffix(".json.tmp")
            tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
            tmp.replace(cache_file)

        d.role = str(data.get("role", ""))[:200]
        d.traits = [str(t) for t in data.get("traits", [])][:6]
        d.speaking_style = str(data.get("speaking_style", d.speaking_style))[:200]
        d.catchphrases = [str(c) for c in data.get("catchphrases", [])][:3]
        d.backstory = str(data.get("backstory", ""))[:600]
        for who, rel in (data.get("relationships") or {}).items():
            d.relationships[str(who)] = str(rel)[:100]
        llm_knowledge = []
        for item in data.get("knowledge", []):
            idx = item.get("scene")
            if isinstance(idx, int) and 0 <= idx < len(scenes):
                llm_knowledge.append(KnowledgeItem(fact=str(item.get("fact", ""))[:300],
                                                   max_order=scenes[idx].max_order))
        if llm_knowledge:
            d.knowledge = llm_knowledge              # refined facts, same spoiler gates
        d.generated_by = "llm"
        d.needs_review = True                        # persona always passes the human gate
