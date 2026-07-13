# Hermes Book-to-Voice Studio

The **offline "Book → Data" half** of the Hermes pipeline. Turns a book file into the **content
bundle** the Hermes device ingests. See `../docs/end_to_end_pipeline.md` for the full architecture;
this directory implements Part A + the §3 bundle contract.

> **Separate build path.** Python-only, its own venv. **Not part of the aarch64 device build** —
> CMake never sees `studio/`. The only thing shared with the device is the **bundle contract**
> (`studio/contract/`), which Hermes mirrors on the ingest side.

## Quick start (zero dependencies — stdlib only)
```bash
cd Hermes
python3 -m studio.build_book studio/samples/lighthouse.txt --out out/books/lighthouse
python3 -m studio.contract.validate out/books/lighthouse
python3 -m unittest studio.tests.test_pipeline -v
```
The rules tiers need **no third-party packages**. Optional higher-fidelity tiers (spaCy coref, LLM
attribution, EPUB ingest) are in `requirements-optional.txt` and enabled by flags.

### Scanned-PDF books (OCR tier)
```bash
brew install poppler tesseract        # external CLI tools; still zero Python deps
python3 -m studio.build_book LADYBIRD_Alladin-1.pdf --title "Aladdin" --out out/books/aladdin
```
Text-layer PDFs go through `pdftotext` directly. Scanned books are rendered per page
(`--ocr-dpi`, default 300), two-page spreads are split at the gutter, each printed page is
OCR'd with word-confidence filtering (art garbage OCRs at conf ≤72, real text ≥86), classified
as story body vs front/back matter (covers, copyright, series blurbs are dropped), and
sentence/quote blocks split by large-print spacing are re-merged. Every printed page is an
independent, idempotent unit whose render+OCR artifacts are cached under
`~/.cache/hermes-studio/ocr/<pdf-sha>-<dpi>-<tess-version>/` (`--ocr-cache` to relocate) — a
re-run is pure cache hits, and a cloud batch runner can fan pages out without changing the code.

## Stages (`docs/end_to_end_pipeline.md` §2)
| Dir | Stage | Rules tier (default) | Optional tier |
|-----|-------|----------------------|---------------|
| `ingest/` | ① ingest & normalize | .txt/.md prose | `.epub` (ebooklib) · **PDF** (`ingest/pdf.py`: text-layer via `pdftotext`, scanned via `pdftoppm`+`tesseract` — CLI tools, no Python deps) |
| `nlp/` | ② character registry | capitalized-name freq + pronoun gender | `--nlp spacy` (NER + coref) |
| `attribute/` | ③ speaker attribution ★ | tiered rules + confidence | `--attributor llm` (sliding window) |
| `emotion/` | ④ emotion tagging | cue words → 7-state | classifier / LLM |
| `casting/` | ⑤ voice casting | seeded, stable map | voice-clone library |
| `knowledge/` | ⑧ scenes + dossiers | scene split (location cues), addressees (vocative/two-party), evidence-only dossiers | `--knowledge llm`: persona authoring via `claude` CLI, content-hash cached |
| `bundle/` | ⑦ bundle writer | emits the §3 contract (v0.2: + `scenes.json`, dossier cards) | — |
| `contract/` | the seam | schema + `audio_key` + validator | — |

Stage ⑥ (TTS synthesis → audio clips) is the GPU/cloud step; not in this MVP. Utterances carry
`audio: null` until rendered, and `audio_key(text, voice, emotion, engine_version)` is the filename
Hermes resolves against its clip cache.

## Output — the content bundle (`docs/end_to_end_pipeline.md` §3)
```
out/books/<id>/
  manifest.json        # counts, contract_version, per-file sha256, audio_rendered:false
  utterances.jsonl     # one row per span: speaker_id, emotion, order, confidence, tier,
                       #   addressee_id·scene_id (v0.2, nullable)
  scenes.json          # v0.2 Layer-3 story knowledge: order ranges, location, participants,
                       #   summary, max_order spoiler gate (UC-8 role-load filters on this)
  characters.okf/      # OKF Character dossier cards → Hermes role-load (persona, relationships,
                       #   quotes, spoiler-gated knowledge; needs_review:true until human-gated)
  casting.json         # character_id → voice_profile_id  (seeded, stable)
  voice_profiles.json  # voice_profile_id → {engine, seed, pitch, rate, style}
  chapters.json · SIGNATURE (unsigned placeholder — real signing deferred, ARCHITECTURE §9.4)
```

## Design notes
- **Cheapest-first, confidence everywhere.** Explicit tags → pronoun/nearest-name → alternating →
  LLM (opt-in). Anything below `--tau` (default 0.75) is a **human-review candidate**, printed at
  the end of a run. Never chase 100% autonomous accuracy — make low-confidence cheap to fix.
- **Deterministic.** No RNG, no wall-clock — ids, seeds, and casting derive from SHA-256, so a
  rebuild is byte-identical (voices never drift between runs). Verified in `tests/`.
- **Honest degradation.** Optional tiers fall back to rules with a printed note; the pipeline never
  fails because a model isn't installed.

## Status
Rules tiers + **PDF ingest (text-layer & scanned/OCR)** + **Stage ⑧ knowledge (scenes,
addressees, dossiers — incl. the `claude`-CLI LLM persona tier)**: **built & tested** — validated
end-to-end on a scanned Ladybird picture book (16 image-only spread scans → contract-v0.2
bundle, all dialogue attributed, front/back matter auto-dropped, 4 scenes with spoiler gates,
LLM dossier cards for all 4 characters, byte-identical rebuilds in both tiers). spaCy/coref, LLM
attribution/emotion, and Stage ⑥ TTS: **extension points, not yet wired**. This is Milestones
M0–M2.5 (Studio half) of `docs/end_to_end_pipeline.md` §9.

Known rules-tier limits found with the Aladdin scan (all by design, ML/LLM tiers refine them):
lowercase common-noun characters ("the genie") are not registered; pronoun-window gender guessing
can mis-vote when the pronoun's antecedent is another character; casting assigns voices to
characters that never speak (harmless — no clips are rendered for them).
