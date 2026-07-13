"""Stage 1 — Ingestion & normalization.  Book file → structured doc (chapters, paragraphs).

Zero-dep for .txt/.md prose. EPUB is an OPTIONAL tier (needs `ebooklib`+`bs4`); if unavailable
we raise a clear message. PDF (text-layer and scanned/OCR) is the poppler+tesseract tier in
`ingest/pdf.py` — external CLI tools, still no Python packages.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

# smart-quote / dash / ellipsis normalization → canonical ASCII forms (Stage 1 of the PDF)
_SMART = {
    "“": '"', "”": '"', "‘": "'", "’": "'",
    "–": "-", "—": "-", "…": "...", " ": " ",
}
_CHAP_RE = re.compile(r"^\s*(chapter|book|part)\s+([ivxlcdm0-9]+)\b", re.IGNORECASE)


@dataclass
class Chapter:
    index: int
    title: str
    paragraphs: list = field(default_factory=list)


@dataclass
class Doc:
    title: str
    author: str
    chapters: list = field(default_factory=list)


def normalize(text: str) -> str:
    for k, v in _SMART.items():
        text = text.replace(k, v)
    text = re.sub(r"(\w)-\n(\w)", r"\1\2", text)     # de-hyphenate across line breaks
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    return text


def _split_paragraphs(block: str) -> list:
    paras = [re.sub(r"\s+", " ", p).strip() for p in re.split(r"\n\s*\n", block)]
    return [p for p in paras if p]


def load_txt(path: Path) -> Doc:
    return doc_from_text(Path(path).read_text(encoding="utf-8"))


def doc_from_text(text: str, default_title: "str | None" = None) -> Doc:
    """Normalized prose → Doc. Shared by the .txt/.md loader and the PDF text-layer tier."""
    raw = normalize(text)
    lines = raw.split("\n")

    # Title/author heuristic: first non-empty line is the title (strip a leading markdown #).
    title, author = default_title or "Untitled", "Unknown"
    if default_title is None:
        for ln in lines:
            if ln.strip():
                title = ln.strip().lstrip("#").strip()
                break

    # Chapter detection: split on heading lines; everything before the first heading is Chapter 1.
    chapters, cur_title, cur_lines, idx = [], None, [], 0

    def flush():
        nonlocal cur_lines, cur_title, idx
        body = "\n".join(cur_lines).strip()
        if body:
            chapters.append(Chapter(index=idx, title=cur_title or f"Chapter {idx + 1}",
                                    paragraphs=_split_paragraphs(body)))
            idx += 1
        cur_lines = []

    # When the title came from the text, drop that first line from the body; when it was
    # supplied by the caller (PDF tier), every line is story text.
    seen_title = default_title is not None
    for ln in lines:
        if not seen_title and ln.strip():
            seen_title = True                        # drop the title line itself from body
            continue
        m = _CHAP_RE.match(ln)
        if m:
            flush()
            cur_title = ln.strip()
            continue
        cur_lines.append(ln)
    flush()

    if not chapters:                                  # no headings at all → single chapter
        body = lines if default_title is not None else lines[1:]
        chapters = [Chapter(0, "Chapter 1", _split_paragraphs("\n".join(body)))]
    return Doc(title=title, author=author, chapters=chapters)


def load_epub(path: Path) -> Doc:
    try:
        import ebooklib
        from ebooklib import epub
        from bs4 import BeautifulSoup
    except ImportError as e:  # optional tier
        raise RuntimeError("EPUB ingest needs `ebooklib` + `beautifulsoup4` "
                            "(pip install ebooklib beautifulsoup4)") from e
    book = epub.read_epub(str(path))
    title = (book.get_metadata("DC", "title") or [["Untitled"]])[0][0]
    author = (book.get_metadata("DC", "creator") or [["Unknown"]])[0][0]
    chapters = []
    for i, item in enumerate(book.get_items_of_type(ebooklib.ITEM_DOCUMENT)):
        soup = BeautifulSoup(item.get_content(), "html.parser")
        body = normalize(soup.get_text("\n"))
        paras = _split_paragraphs(body)
        if paras:
            chapters.append(Chapter(index=len(chapters), title=f"Chapter {len(chapters)+1}",
                                    paragraphs=paras))
    return Doc(title=title, author=author, chapters=chapters)


def load_book(path: str, *, title: "str | None" = None, author: "str | None" = None,
              ocr_dpi: int = 300, ocr_cache: "str | None" = None) -> Doc:
    p = Path(path)
    ext = p.suffix.lower()
    if ext in (".txt", ".md"):
        doc = load_txt(p)
    elif ext == ".epub":
        doc = load_epub(p)
    elif ext == ".pdf":
        from studio.ingest.pdf import load_pdf     # lazy: keeps txt/epub path tool-free
        return load_pdf(p, dpi=ocr_dpi, cache_dir=ocr_cache, title=title, author=author)
    else:
        raise RuntimeError(f"unsupported input {ext!r}; supported: .txt .md .epub .pdf")
    if title:
        doc.title = title
    if author:
        doc.author = author
    return doc
