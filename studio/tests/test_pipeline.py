"""Smoke + behaviour tests for the Studio rules pipeline. Stdlib unittest (no deps).

    python -m unittest studio.tests.test_pipeline -v
"""
import json
import tempfile
import unittest
from pathlib import Path

from studio.build_book import build
from studio.contract import audio_key
from studio.contract.validate import validate_bundle

SAMPLE = str(Path(__file__).resolve().parents[1] / "samples" / "lighthouse.txt")


class TestPipeline(unittest.TestCase):
    def _build(self, out):
        return build(SAMPLE, out)

    def test_bundle_is_contract_valid(self):
        with tempfile.TemporaryDirectory() as d:
            self._build(d)
            self.assertEqual(validate_bundle(d), [])

    def test_attribution_and_registry(self):
        with tempfile.TemporaryDirectory() as d:
            self._build(d)
            utts = [json.loads(l) for l in (Path(d) / "utterances.jsonl").read_text().splitlines()]
            casting = json.loads((Path(d) / "casting.json").read_text())
            # narrator + two cast characters
            self.assertEqual(len([c for c in casting if c != "NARRATOR"]), 2)
            # global order is a dense monotonic sequence
            self.assertEqual([u["order"] for u in utts], list(range(len(utts))))
            # a known explicit line is attributed to a real (non-narrator/unknown) character
            line = next(u for u in utts if u["text"].startswith("You shouldn't be up here"))
            self.assertEqual(line["tier"], "explicit")
            self.assertNotIn(line["speaker_id"], ("NARRATOR", "UNKNOWN"))
            # emotion vocabulary is respected
            for u in utts:
                self.assertIn(u["emotion"]["label"],
                              ("neutral", "joy", "sadness", "anger", "fear", "surprise", "tenderness"))

    def test_determinism(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            self._build(a)
            self._build(b)
            self.assertEqual((Path(a) / "utterances.jsonl").read_text(),
                             (Path(b) / "utterances.jsonl").read_text())
            self.assertEqual((Path(a) / "casting.json").read_text(),
                             (Path(b) / "casting.json").read_text())

    def test_rebuild_into_same_dir_is_idempotent(self):
        # stale outputs from a previous build (e.g. a since-dropped character's .md, the old
        # SIGNATURE) must not leak into the new manifest
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "characters.okf").mkdir()
            (Path(d) / "characters.okf" / "ch_stale123.md").write_text("old")
            (Path(d) / "SIGNATURE").write_text("unsigned-sha256:stale")
            manifest = self._build(d)
            self.assertEqual(validate_bundle(d), [])
            self.assertFalse((Path(d) / "characters.okf" / "ch_stale123.md").exists())
            self.assertNotIn("SIGNATURE", manifest["files"])

    def test_audio_key_is_stable_and_field_separated(self):
        k1 = audio_key("hi", "vp_x", "joy", "xtts-2")
        k2 = audio_key("hi", "vp_x", "joy", "xtts-2")
        self.assertEqual(k1, k2)                       # deterministic
        # field boundaries matter: "a|b" must not collide with "ab|"
        self.assertNotEqual(audio_key("ab", "c", "joy", "v"), audio_key("a", "bc", "joy", "v"))


if __name__ == "__main__":
    unittest.main()
