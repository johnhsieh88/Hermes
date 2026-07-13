"""Stage 7 (data) — write the CONTENT BUNDLE: the §3 contract Hermes ingests.

Produces the on-disk layout of docs/end_to_end_pipeline.md §3 (minus the audio clips, which are
Stage 6/GPU): manifest.json, utterances.jsonl, characters.okf/, casting.json, voice_profiles.json,
chapters.json. Integrity = sha256 per file in the manifest; a real detached SIGNATURE over the
manifest is deferred (device verifies it before install — Part I §9.4).
"""
from __future__ import annotations

import hashlib
import json
from dataclasses import asdict
from pathlib import Path

from studio.contract import (CONTRACT_VERSION, STUDIO_VERSION, slugify, NARRATOR)


def _sha256_file(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def _character_okf(c, d=None) -> str:
    """OKF Character card. With a Dossier (contract v0.2) this is the full role-load card
    the device assembles into the UC-8 system prompt; without one it degrades to the v0.1
    identity stub."""
    aliases = ", ".join(c.aliases)
    head = (f"---\n"
            f"type: Character\n"
            f"canonical_name: {c.canonical_name}\n"
            f"aliases: [{aliases}]\n"
            f"gender: {c.gender}\n"
            f"voice_profile_id: {c.voice_profile_id}\n"
            f"mention_count: {c.mention_count}\n")
    if d is None:
        return (head + "---\n\n"
                f"# {c.canonical_name}\n\n"
                f"Speaking character extracted for the audiobook. Cast to voice profile "
                f"`{c.voice_profile_id}`.\n")

    head += (f"role: {d.role}\n"
             f"traits: [{', '.join(d.traits)}]\n"
             f"emotion_range: [{', '.join(d.emotion_range)}]\n"
             f"generated_by: {d.generated_by}\n"
             f"needs_review: {str(d.needs_review).lower()}\n"
             f"---\n\n")
    body = [f"# {c.canonical_name}"]
    if d.role:
        body += ["", f"**Role:** {d.role}"]
    if d.backstory:
        body += ["", f"**Backstory:** {d.backstory}"]
    if d.speaking_style:
        body += ["", f"**Speaking style:** {d.speaking_style}"]
    if d.catchphrases:
        body += ["", f"**Catchphrases:** " + " · ".join(f"“{p}”" for p in d.catchphrases)]
    if d.relationships:
        body += ["", "## Relationships"] + [f"- **{who}** — {rel}"
                                            for who, rel in d.relationships.items()]
    if d.quotes:
        body += ["", "## Lines from the book"] + [f"- “{q}”" for q in d.quotes]
    if d.knowledge:
        body += ["", "## Knowledge (spoiler-gated: reveal only when pos > max_order)"]
        body += [f"- (max_order={k.max_order}) {k.fact}" for k in d.knowledge]
    return head + "\n".join(body) + "\n"


_STUDIO_FILES = ("manifest.json", "SIGNATURE", "utterances.jsonl", "casting.json",
                 "voice_profiles.json", "chapters.json", "scenes.json")


def write_bundle(out_dir, doc, characters, utterances, casting, profiles,
                 scenes=None, dossiers=None, engine_version: str = "unrendered") -> dict:
    out = Path(out_dir)
    book_id = f"{slugify(doc.title)}-{hashlib.sha256(doc.title.encode()).hexdigest()[:8]}"
    (out / "characters.okf").mkdir(parents=True, exist_ok=True)
    (out / "audio").mkdir(parents=True, exist_ok=True)  # populated by Stage 6 later

    # Idempotent re-run: remove studio-owned outputs from any previous build, else stale
    # files (e.g. a dropped character's .md, last run's SIGNATURE) get hashed into the new
    # manifest. audio/ is deliberately kept — Stage 6 clips are content-addressed by
    # audio_key and expensive to re-render.
    for name in _STUDIO_FILES:
        (out / name).unlink(missing_ok=True)
    for stale in (out / "characters.okf").glob("*.md"):
        stale.unlink()

    # utterances.jsonl (one row per span, ordered)
    with (out / "utterances.jsonl").open("w", encoding="utf-8") as f:
        for u in utterances:
            f.write(json.dumps(u.to_json(), ensure_ascii=False) + "\n")

    # characters.okf/*.md (OKF Character docs → Hermes curated knowledge / UC-8 role-load)
    for c in characters:
        d = (dossiers or {}).get(c.id)
        (out / "characters.okf" / f"{c.id}.md").write_text(_character_okf(c, d),
                                                           encoding="utf-8")

    # scenes.json (Layer-3 story knowledge; spoiler gates for role-load)
    (out / "scenes.json").write_text(
        json.dumps([asdict(s) for s in (scenes or [])], indent=2, ensure_ascii=False),
        encoding="utf-8")

    (out / "casting.json").write_text(json.dumps(casting, indent=2), encoding="utf-8")
    (out / "voice_profiles.json").write_text(
        json.dumps({k: asdict(v) for k, v in profiles.items()}, indent=2), encoding="utf-8")
    (out / "chapters.json").write_text(json.dumps(
        [{"index": ch.index, "title": ch.title, "paragraphs": len(ch.paragraphs)}
         for ch in doc.chapters], indent=2), encoding="utf-8")

    n_unknown = sum(1 for u in utterances if u.speaker_id == "UNKNOWN")
    n_low = sum(1 for u in utterances if u.confidence < 0.75)
    manifest = {
        "book_id": book_id,
        "title": doc.title,
        "author": doc.author,
        "contract_version": CONTRACT_VERSION,
        "studio_version": STUDIO_VERSION,
        "engine_version": engine_version,
        "audio_rendered": False,
        "counts": {
            "chapters": len(doc.chapters),
            "characters": len(characters),
            "scenes": len(scenes or []),
            "utterances": len(utterances),
            "narration": sum(1 for u in utterances if u.speaker_id == NARRATOR),
            "dialogue": sum(1 for u in utterances if u.speaker_id not in (NARRATOR, "UNKNOWN")),
            "unknown_speaker": n_unknown,
            "below_tau": n_low,
        },
        "files": {},
    }
    # integrity hashes for every emitted file (except the manifest itself)
    for p in sorted(out.rglob("*")):
        if p.is_file() and p.name != "manifest.json":
            manifest["files"][str(p.relative_to(out))] = _sha256_file(p)

    (out / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    # SIGNATURE: placeholder digest over the manifest; real signing (Part I §9.4) is deferred.
    sig = hashlib.sha256((out / "manifest.json").read_bytes()).hexdigest()
    (out / "SIGNATURE").write_text(f"unsigned-sha256:{sig}\n", encoding="utf-8")
    return manifest
