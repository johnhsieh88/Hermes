"""Book → Data → Bundle: the end-to-end Studio driver (rules tiers by default).

    python -m studio.build_book studio/samples/lighthouse.txt --out books/lighthouse

Runs stages 1–5 + bundle write + contract validation, then prints a summary and the low-confidence
attributions a human-review UI would surface. ML/LLM tiers are opt-in flags.
"""
from __future__ import annotations

import argparse
from pathlib import Path

from studio.ingest import load_book
from studio.nlp import build_registry
from studio.attribute import attribute_doc
from studio.casting import build_casting
from studio.knowledge import (assign_addressees, build_dossiers, build_scenes,
                              summarize_scenes)
from studio.bundle import write_bundle
from studio.contract.validate import validate_bundle


def build(input_path: str, out_dir: str, *, nlp="rules", attributor="rules",
          engine="xtts-v2", tau=0.75, title=None, author=None,
          ocr_dpi=300, ocr_cache=None, knowledge="rules") -> dict:
    print(f"① ingest      {input_path}")
    doc = load_book(input_path, title=title, author=author,
                    ocr_dpi=ocr_dpi, ocr_cache=ocr_cache)
    print(f"   → {len(doc.chapters)} chapter(s), "
          f"{sum(len(c.paragraphs) for c in doc.chapters)} paragraph(s): {doc.title!r}")

    print(f"② registry    (nlp={nlp})")
    characters = build_registry(doc, nlp=nlp)
    for c in characters:
        print(f"   • {c.canonical_name:12} {c.gender:7} ×{c.mention_count}")

    print(f"③ attribute   (attributor={attributor})")
    utterances = attribute_doc(doc, characters, attributor=attributor)

    print("⑤ casting     (seeded, stable)")
    casting, profiles = build_casting(characters, engine=engine)

    print(f"⑧ knowledge   (knowledge={knowledge})")
    scenes = build_scenes(utterances, characters)
    summarize_scenes(scenes, utterances)
    assign_addressees(utterances, characters, scenes)
    dossiers = build_dossiers(doc, characters, utterances, scenes, knowledge=knowledge)
    for s in scenes:
        print(f"   • {s.scene_id} orders {s.order_start}–{s.order_end} "
              f"loc={s.location:8} participants={len(s.participants)}")

    print(f"⑦ bundle      → {out_dir}")
    manifest = write_bundle(out_dir, doc, characters, utterances, casting, profiles,
                            scenes=scenes, dossiers=dossiers)

    errs = validate_bundle(out_dir)
    cc = manifest["counts"]
    print("\n── summary ─────────────────────────────────────────────")
    print(f"   utterances {cc['utterances']}  (narration {cc['narration']}, dialogue {cc['dialogue']})")
    print(f"   characters {cc['characters']}   unknown-speaker {cc['unknown_speaker']}   "
          f"below τ={tau} {cc['below_tau']}")
    print(f"   contract   {'VALID ✅' if not errs else 'INVALID ❌ ' + str(errs)}")

    low = [u for u in utterances if u.confidence < tau]
    if low:
        print(f"\n── review queue (confidence < {tau}) — {len(low)} item(s) ──")
        for u in low[:12]:
            print(f"   ch{u.chapter} #{u.order} [{u.tier}/{u.confidence:.2f}] "
                  f"{u.speaker_id:8} “{u.text[:52]}”")
    return manifest


def main():
    ap = argparse.ArgumentParser(description="Book → data bundle for Hermes (rules tiers by default)")
    ap.add_argument("input", help="book file (.txt/.md/.epub/.pdf)")
    ap.add_argument("--out", required=True, help="output bundle directory")
    ap.add_argument("--nlp", choices=["rules", "spacy"], default="rules")
    ap.add_argument("--attributor", choices=["rules", "llm"], default="rules")
    ap.add_argument("--engine", default="xtts-v2", help="target TTS engine id (for casting)")
    ap.add_argument("--tau", type=float, default=0.75, help="review threshold")
    ap.add_argument("--title", help="book title override (PDF filenames are poor titles)")
    ap.add_argument("--author", help="book author override")
    ap.add_argument("--ocr-dpi", type=int, default=300, help="render DPI for scanned-PDF OCR")
    ap.add_argument("--ocr-cache", help="per-page OCR artifact cache dir "
                                        "(default ~/.cache/hermes-studio/ocr)")
    ap.add_argument("--knowledge", choices=["rules", "llm"], default="rules",
                    help="Stage 8 dossier tier (llm = persona authoring via `claude` CLI)")
    a = ap.parse_args()
    build(a.input, a.out, nlp=a.nlp, attributor=a.attributor, engine=a.engine, tau=a.tau,
          title=a.title, author=a.author, ocr_dpi=a.ocr_dpi, ocr_cache=a.ocr_cache,
          knowledge=a.knowledge)


if __name__ == "__main__":
    main()
