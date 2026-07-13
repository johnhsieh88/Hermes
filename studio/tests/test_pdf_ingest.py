"""Unit tests for the PDF/OCR ingest tier — pure logic only, no poppler/tesseract needed.

The OCR-dependent path is covered by the (skippable) integration test at the bottom, which
runs only when the tools and a scanned sample PDF are both present.
"""
import shutil
import unittest
from pathlib import Path

from studio.ingest.pdf import (PageText, assemble_doc, classify_page, clean_ocr_text,
                               text_from_tsv, _find_author)
from studio.nlp import build_registry_rules
from studio.ingest import Doc, Chapter


def _tsv(rows):
    head = "level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext"
    return head + "\n" + "\n".join(
        f"5\t1\t{b}\t{p}\t{ln}\t{w}\t0\t0\t10\t10\t{conf}\t{word}"
        for b, p, ln, w, conf, word in rows)


class TestTsvExtraction(unittest.TestCase):
    def test_confidence_gate_drops_art_garbage(self):
        tsv = _tsv([(1, 1, 1, 1, 96.1, "Once"), (1, 1, 1, 2, 95.0, "upon"),
                    (2, 1, 1, 1, 30.2, "V/"), (2, 1, 1, 2, 23.2, "Sag")])
        self.assertEqual(text_from_tsv(tsv), "Once upon")

    def test_letterless_words_dropped_even_when_confident(self):
        tsv = _tsv([(1, 1, 1, 1, 96.0, "Hello"), (1, 1, 1, 2, 95.0, "|")])
        self.assertEqual(text_from_tsv(tsv), "Hello")

    def test_paragraph_and_line_structure_rebuilt(self):
        tsv = _tsv([(1, 1, 1, 1, 96.0, "line"), (1, 1, 1, 2, 96.0, "one"),
                    (1, 1, 2, 1, 96.0, "line"), (1, 1, 2, 2, 96.0, "two"),
                    (1, 2, 1, 1, 96.0, "para"), (1, 2, 1, 2, 96.0, "two")])
        self.assertEqual(text_from_tsv(tsv), "line one\nline two\n\npara two")


class TestOcrCleanup(unittest.TestCase):
    def test_quote_capital_i_misread_as_t(self):
        self.assertEqual(clean_ocr_text("“T wish to be a prince,” said Aladdin."),
                         "“I wish to be a prince,” said Aladdin.")

    def test_page_numbers_and_symbol_lines_dropped(self):
        self.assertEqual(clean_ocr_text("He rubbed the lamp.\n12\ni >)\n"),
                         "He rubbed the lamp.")


class TestPageClassification(unittest.TestCase):
    def test_copyright_page_is_matter(self):
        self.assertEqual(classify_page(
            "Published by Ladybird Books Ltd\nCopyright © 2004 Disney\n"
            "All rights reserved."), "matter")

    def test_series_blurb_and_back_cover_are_matter(self):
        self.assertEqual(classify_page(
            "Read it yourself is a series of graded readers designed for children."),
            "matter")
        self.assertEqual(classify_page("www.ladybird.co.uk"), "matter")

    def test_story_prose_is_body(self):
        self.assertEqual(classify_page(
            "Once there was a poor boy called Aladdin. He lived in Agrabah."), "body")

    def test_near_blank_page_is_empty(self):
        self.assertEqual(classify_page(""), "empty")
        self.assertEqual(classify_page("ee"), "empty")


class TestAssembly(unittest.TestCase):
    def test_matter_and_empty_pages_excluded(self):
        pages = [PageText(1, "F", "Copyright © 2004 Disney ISBN", "matter"),
                 PageText(2, "L", "Once there was a boy.", "body"),
                 PageText(2, "R", "", "empty")]
        doc = assemble_doc(pages, title="T", author="A")
        self.assertEqual(doc.chapters[0].paragraphs, ["Once there was a boy."])

    def test_quote_split_across_blocks_is_merged(self):
        pages = [PageText(1, "R", "“I wish to be\n\na prince,” said\nAladdin.", "body")]
        doc = assemble_doc(pages, title="T", author="A")
        self.assertEqual(doc.chapters[0].paragraphs,
                         ['"I wish to be a prince," said Aladdin.'])

    def test_sentence_split_across_pages_is_merged(self):
        pages = [PageText(1, "L", "He lived in", "body"),
                 PageText(1, "R", "the city of Agrabah.", "body"),
                 PageText(2, "L", "A new sentence.", "body")]
        doc = assemble_doc(pages, title="T", author="A")
        self.assertEqual(doc.chapters[0].paragraphs,
                         ["He lived in the city of Agrabah.", "A new sentence."])

    def test_all_pages_matter_raises_clear_error(self):
        with self.assertRaises(RuntimeError):
            assemble_doc([PageText(1, "F", "ISBN 123", "matter")], title="T", author="A")

    def test_author_from_copyright_line(self):
        pages = [PageText(1, "F", "Published by X\nCopyright © 2004 Disney", "matter")]
        self.assertEqual(_find_author(pages), "Disney")


class TestRegistryFixes(unittest.TestCase):
    def _doc(self, paras):
        return Doc(title="T", author="A", chapters=[Chapter(0, "C1", paras)])

    def test_place_after_of_is_not_a_character(self):
        doc = self._doc(["Aladdin lived in the city of Agrabah.",
                         "The Sultan of Agrabah liked Aladdin."])
        names = {c.canonical_name for c in build_registry_rules(doc)}
        self.assertIn("Aladdin", names)
        self.assertNotIn("Agrabah", names)

    def test_gendered_title_used_as_name(self):
        doc = self._doc(["The Sultan told Jasmine that she must marry.",
                         "Jasmine told the Sultan that she wanted to marry."])
        by_name = {c.canonical_name: c for c in build_registry_rules(doc)}
        self.assertEqual(by_name["Sultan"].gender, "male")
        self.assertEqual(by_name["Jasmine"].gender, "female")


@unittest.skipUnless(shutil.which("tesseract") and shutil.which("pdftoppm")
                     and (Path(__file__).parents[2] / "LADYBIRD_Alladin-1.pdf").exists(),
                     "needs poppler+tesseract and the local sample scan")
class TestScannedPdfIntegration(unittest.TestCase):
    def test_aladdin_end_to_end_ingest(self):
        from studio.ingest import load_book
        doc = load_book(str(Path(__file__).parents[2] / "LADYBIRD_Alladin-1.pdf"),
                        title="Aladdin")
        paras = doc.chapters[0].paragraphs
        self.assertTrue(paras[0].startswith("Once there was a poor boy called Aladdin"))
        self.assertEqual(sum(p.count('"') for p in paras) % 2, 0)   # quotes all balanced
        joined = " ".join(paras)
        self.assertIn('"I wish to be a prince," said Aladdin.', joined)
        self.assertNotIn("ISBN", joined)                            # matter filtered out
        self.assertNotIn("Ladybird", joined)


if __name__ == "__main__":
    unittest.main()
