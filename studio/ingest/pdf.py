"""Stage 1 (PDF tier) — PDF → structured Doc, for both text-layer and scanned/image books.

Zero *Python* dependencies: shells out to poppler (`pdfinfo`/`pdftotext`/`pdftoppm`) and
`tesseract`. Missing tools raise a clear install hint instead of failing mid-run.

Two paths, cheapest first (same tiering philosophy as the rest of the studio):
  1. text-layer PDF  → `pdftotext` (fast, exact); used when the PDF embeds enough text.
  2. scanned PDF     → per-page OCR: render with `pdftoppm`, split two-page spreads at the
                       gutter (landscape pages), OCR each printed page top-to-bottom, classify
                       every printed page as body / front-or-back matter / empty, keep body only.

Cloud-ready shaping: each printed page is an independent, idempotent unit of work whose
rendered image + OCR text are cached under `<cache>/<pdf-sha12>/pNNN[LRF]@DPI.{png,txt}`,
keyed by content hash + DPI + tesseract version — re-runs are cache hits, and a batch runner
can fan pages out in parallel later without changing this module.
"""
from __future__ import annotations

import hashlib
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

from studio.ingest import Chapter, Doc, doc_from_text, normalize

_DEF_CACHE = Path.home() / ".cache" / "hermes-studio" / "ocr"
_TEXT_LAYER_MIN_CHARS = 200          # below this, treat the PDF as scanned → OCR path
_SPREAD_RATIO = 1.05                 # width > height*ratio → two-page spread, split at gutter
_EMPTY_MIN_CHARS = 15                # a printed page with less text than this is art/blank
_MERGE_MAX_CHARS = 1200              # stop merging "unfinished" paragraphs beyond this

# One hit on any of these marks a printed page as front/back matter, not story body.
# Conservative on purpose: publisher/legal/series boilerplate, not words a story would use.
_MATTER_RE = re.compile(
    r"isbn|copyright|all rights reserved|published by|printed in|trademark"
    r"|www\.|\.co\.uk|\.com\b|read it yourself|graded reader|about this book"
    r"|series of|suitable for children|essential vocabulary|penguin books|ladybird",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class PageText:
    """OCR result for one printed page (a spread half or a full single page)."""
    pdf_page: int                     # 1-based PDF page
    half: str                         # "L" | "R" | "F" (full page)
    text: str
    kind: str                         # "body" | "matter" | "empty"


# ── tool plumbing ────────────────────────────────────────────────────────────

def _require(cmd: str, hint: str) -> str:
    path = shutil.which(cmd)
    if not path:
        raise RuntimeError(f"PDF ingest needs `{cmd}` on PATH ({hint})")
    return path


_TOOL_TIMEOUT = 300          # s per external call; a stuck pdftoppm/tesseract on one bad
                             # page must not wedge a whole batch ingest run

def _run(args: list) -> str:
    # encoding pinned to UTF-8: `text=True` alone decodes with the host locale, which on a
    # LANG=C CI box would crash (or silently differ) on the first non-ASCII character —
    # breaking the byte-identical-rebuild guarantee across machines.
    try:
        return subprocess.run(args, capture_output=True, encoding="utf-8",
                              check=True, timeout=_TOOL_TIMEOUT).stdout
    except subprocess.CalledProcessError as e:
        raise RuntimeError(f"`{args[0]}` failed ({e.returncode}): "
                           f"{(e.stderr or '').strip()[:400]}") from e
    except subprocess.TimeoutExpired as e:
        raise RuntimeError(f"`{args[0]}` timed out after {_TOOL_TIMEOUT}s "
                           f"(malformed page?): {' '.join(args[:6])}…") from e


def _tesseract_version() -> str:
    out = subprocess.run(["tesseract", "--version"], capture_output=True,
                         encoding="utf-8", timeout=_TOOL_TIMEOUT)
    m = re.search(r"tesseract\s+([\w.]+)", out.stdout + out.stderr)
    return m.group(1) if m else "unknown"


def _poppler_version() -> str:
    out = subprocess.run(["pdftoppm", "-v"], capture_output=True,
                         encoding="utf-8", timeout=_TOOL_TIMEOUT)
    m = re.search(r"version\s+([\w.]+)", out.stdout + out.stderr)
    return m.group(1) if m else "unknown"


def _pdf_pages(path: Path) -> list:
    """[(page_no, width_pts, height_pts)] via `pdfinfo -f 1 -l -1`."""
    out = _run(["pdfinfo", "-f", "1", "-l", "-1", str(path)])
    pages = []
    for m in re.finditer(r"Page\s+(\d+)\s+size:\s+([\d.]+)\s+x\s+([\d.]+)\s+pts", out):
        pages.append((int(m.group(1)), float(m.group(2)), float(m.group(3))))
    if not pages:
        raise RuntimeError(f"pdfinfo returned no page sizes for {path}")
    return pages


# ── OCR text clean-up (pure, unit-tested) ────────────────────────────────────

_QUOTE_T = re.compile(r"([“\"'‘])T(?=\s+[a-z])")       # OCR misreads “I → “T before lowercase
_NUM_ONLY = re.compile(r"^[\d\s.\-–—]*$")              # printed page numbers / stray digits


def clean_ocr_text(text: str) -> str:
    """Fix known OCR quirks and drop non-text noise lines (art artifacts, page numbers)."""
    text = _QUOTE_T.sub(r"\g<1>I", text)
    kept = []
    for line in text.split("\n"):
        s = line.strip()
        if not s:
            kept.append("")                            # preserve paragraph breaks
            continue
        if _NUM_ONLY.match(s):
            continue                                   # page number embedded in the art
        letters = sum(c.isalpha() for c in s)
        if letters / len(s) < 0.5:
            continue                                   # OCR garbage from illustrations: "i >)"
        kept.append(s)
    return "\n".join(kept).strip()


def classify_page(text: str) -> str:
    """'empty' | 'matter' (front/back boilerplate) | 'body' (story prose)."""
    if len(text) < _EMPTY_MIN_CHARS:
        return "empty"
    if _MATTER_RE.search(text):
        return "matter"
    return "body"


# ── per-page render + OCR, content-hash cached ───────────────────────────────

def _render_half(pdf: Path, page: int, x_px: int, w_px: int, out_png: Path, dpi: int):
    # render to a .tmp name, then atomically rename: a killed run must never leave a
    # truncated PNG that a later run would trust as a cache hit
    tmp_prefix = out_png.parent / (out_png.stem + ".tmp")   # pdftoppm appends ".png"
    _run(["pdftoppm", "-r", str(dpi), "-png", "-f", str(page), "-l", str(page),
          "-x", str(x_px), "-y", "0", "-W", str(w_px), "-singlefile",
          str(pdf), str(tmp_prefix)])
    Path(str(tmp_prefix) + ".png").replace(out_png)


def _write_atomic(path: Path, text: str):
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(text, encoding="utf-8")
    tmp.replace(path)


_WORD_CONF_MIN = 75          # tesseract word confidence: real text ≥86, art garbage ≤72


def _ocr_png(png: Path) -> str:
    return _run(["tesseract", str(png), "stdout", "tsv"])


def text_from_tsv(tsv: str, conf_min: float = _WORD_CONF_MIN) -> str:
    """Tesseract TSV → text, keeping only confident words that contain a letter.

    Illustrations OCR into low-confidence junk ("V/", "ae)", "<4"); the confidence gate is
    what makes scanned picture books usable. Paragraph structure is rebuilt from the TSV
    block/par/line numbering (paragraphs separated by blank lines).
    """
    paras = {}                                       # (block, par) -> {line -> [words]}
    for row in tsv.split("\n")[1:]:
        cols = row.split("\t")
        if len(cols) < 12 or cols[0] != "5":         # level 5 = word
            continue
        word = cols[11].strip()
        if not word or not any(c.isalpha() for c in word):
            continue
        if float(cols[10]) < conf_min:
            continue
        key = (int(cols[2]), int(cols[3]))
        paras.setdefault(key, {}).setdefault(int(cols[4]), []).append(word)
    out = []
    for key in sorted(paras):
        lines = [" ".join(paras[key][ln]) for ln in sorted(paras[key])]
        out.append("\n".join(lines))
    return "\n\n".join(out)


def ocr_pdf(path: Path, *, dpi: int = 300, cache_dir: Optional[Path] = None,
            log=print) -> list:
    """OCR every printed page of a scanned PDF → ordered [PageText]. Cached per page."""
    _require("pdftoppm", "brew install poppler / apt install poppler-utils")
    _require("tesseract", "brew install tesseract / apt install tesseract-ocr")

    sha = hashlib.sha256(path.read_bytes()).hexdigest()[:12]
    cache = (Path(cache_dir or _DEF_CACHE)
             / f"{sha}-{dpi}dpi-tess{_tesseract_version()}-poppler{_poppler_version()}")
    cache.mkdir(parents=True, exist_ok=True)

    results = []
    for page, w_pts, h_pts in _pdf_pages(path):
        w_px = round(w_pts / 72 * dpi)
        if w_pts > h_pts * _SPREAD_RATIO:              # landscape scan = two-page spread
            # split at the pixel midpoint; an off-center binding gutter can bleed a sliver
            # of the facing page into each crop — acceptable, the confidence gate drops it
            halves = [("L", 0, w_px // 2), ("R", w_px // 2, w_px - w_px // 2)]
        else:
            halves = [("F", 0, w_px)]
        for half, x, w in halves:
            tsv_file = cache / f"p{page:03d}{half}.tsv"
            if tsv_file.exists():
                tsv = tsv_file.read_text(encoding="utf-8")
            else:
                png = cache / f"p{page:03d}{half}.png"
                if not png.exists():
                    _render_half(path, page, x, w, png, dpi)
                tsv = _ocr_png(png)
                _write_atomic(tsv_file, tsv)
            text = clean_ocr_text(text_from_tsv(tsv))
            results.append(PageText(page, half, text, classify_page(text)))
    return results


# ── assembly (pure, unit-tested) ─────────────────────────────────────────────

def assemble_doc(pages: list, title: str, author: str) -> Doc:
    """Ordered [PageText] → single-chapter Doc from body pages only.

    Large-print spacing makes OCR split one sentence (or even one quotation) into several
    blocks — so a paragraph is merged with the next while its quotes are unbalanced or it
    doesn't yet end in terminal punctuation.
    """
    paragraphs = []
    for pt in pages:
        if pt.kind != "body":
            continue
        for block in re.split(r"\n\s*\n", normalize(pt.text)):
            para = re.sub(r"\s+", " ", block).strip()
            if not para:
                continue
            prev = paragraphs[-1] if paragraphs else ""
            unfinished = prev and (prev.count('"') % 2 == 1 or prev[-1] not in '."!?:…')
            # cap: one OCR-dropped quote glyph must not cascade the rest of the book
            # into a single run-on paragraph
            if unfinished and len(prev) < _MERGE_MAX_CHARS:
                paragraphs[-1] = prev + " " + para
            else:
                paragraphs.append(para)
    if not paragraphs:
        raise RuntimeError("no story text found: every page classified as matter/empty "
                           "(is this the right book, or does OCR need a higher --ocr-dpi?)")
    return Doc(title=title, author=author,
               chapters=[Chapter(index=0, title="Chapter 1", paragraphs=paragraphs)])


def _find_author(pages: list) -> str:
    """Copyright holder from front matter: 'Copyright © 2004 Disney' → 'Disney'."""
    for pt in pages:
        m = re.search(r"Copyright\s*©?\s*\d{4}\s+([A-Z][\w&. ]{1,40}?)\s*$",
                      pt.text, re.MULTILINE)
        if m:
            return m.group(1).strip()
    return "Unknown"


def _title_from_stem(path: Path) -> str:
    return re.sub(r"[_\-]+", " ", path.stem).strip().title()


# ── entry point ──────────────────────────────────────────────────────────────

def load_pdf(path: Path, *, dpi: int = 300, cache_dir: Optional[Path] = None,
             title: Optional[str] = None, author: Optional[str] = None, log=print) -> Doc:
    _require("pdfinfo", "brew install poppler / apt install poppler-utils")
    _require("pdftotext", "brew install poppler / apt install poppler-utils")

    embedded = _run(["pdftotext", "-enc", "UTF-8", str(path), "-"])
    if len(embedded.strip()) >= _TEXT_LAYER_MIN_CHARS:
        log(f"   [pdf] text layer found ({len(embedded)} chars) → pdftotext path")
        return doc_from_text(embedded.replace("\f", "\n\n"),
                             default_title=title or _title_from_stem(path))

    log("   [pdf] no text layer → OCR path (poppler + tesseract)")
    pages = ocr_pdf(path, dpi=dpi, cache_dir=cache_dir, log=log)
    for pt in pages:
        if pt.kind != "body":
            preview = pt.text.split("\n")[0][:40] if pt.text else ""
            log(f"   [pdf] p{pt.pdf_page:02d}{pt.half} → {pt.kind:6} (skipped) {preview!r}")
    doc = assemble_doc(pages, title=title or _title_from_stem(path),
                       author=author or _find_author(pages))
    if title is None:
        log(f"   [pdf] title from filename: {doc.title!r} (override with --title)")
    return doc
