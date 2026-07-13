"""Unit tests for Stage 8 — scenes (Layer 3), addressees, dossiers (Layer 2). Rules tier only;
the LLM tier is exercised manually (cached, non-hermetic) and always leaves needs_review=True.
"""
import unittest

from studio.contract import Character, Emotion, Utterance, NARRATOR
from studio.knowledge import (assign_addressees, build_dossiers, build_scenes,
                              summarize_scenes)


def _u(order, text, speaker=NARRATOR, chapter=0):
    return Utterance(id=f"u{order}", chapter=chapter, order=order, text=text,
                     speaker_id=speaker, emotion=Emotion(), confidence=1.0)


def _chars():
    return [Character(id="ch_a", canonical_name="Aladdin", aliases=["Aladdin"], gender="male"),
            Character(id="ch_j", canonical_name="Jafar", aliases=["Jafar"], gender="male")]


class TestScenes(unittest.TestCase):
    def test_location_shift_starts_scene_and_stamps_ids(self):
        utts = [_u(0, "Aladdin lived in the city of Agrabah."),
                _u(1, "He was poor."),
                _u(2, "Jafar took Aladdin to a cave."),
                _u(3, "It was dark.")]
        scenes = build_scenes(utts, _chars())
        self.assertEqual([(s.order_start, s.order_end, s.location) for s in scenes],
                         [(0, 1, "city"), (2, 3, "cave")])
        self.assertEqual(utts[0].scene_id, scenes[0].scene_id)
        self.assertEqual(utts[3].scene_id, scenes[1].scene_id)

    def test_max_order_is_scene_end_and_participants_collected(self):
        utts = [_u(0, "Aladdin met Jafar in the city of Agrabah."),
                _u(1, "Hello,", speaker="ch_a")]
        scenes = build_scenes(utts, _chars())
        self.assertEqual(scenes[0].max_order, 1)
        self.assertEqual(set(scenes[0].participants), {"ch_a", "ch_j"})

    def test_chapter_boundary_always_breaks(self):
        utts = [_u(0, "First chapter.", chapter=0), _u(1, "Second chapter.", chapter=1)]
        self.assertEqual(len(build_scenes(utts, _chars())), 2)

    def test_location_named_mid_scene_does_not_split(self):
        # regression: an unnamed scene must be NAMED by its first location cue, not split
        utts = [_u(0, "Aladdin woke up."),
                _u(1, "He went into the market."),
                _u(2, "He bought bread.")]
        scenes = build_scenes(utts, _chars())
        self.assertEqual([(s.order_start, s.order_end, s.location) for s in scenes],
                         [(0, 2, "market")])

    def test_summary_is_first_narration_sentence(self):
        utts = [_u(0, "Aladdin lived in the city of Agrabah. He was poor.")]
        scenes = build_scenes(utts, _chars())
        summarize_scenes(scenes, utts)
        self.assertEqual(scenes[0].summary, "Aladdin lived in the city of Agrabah.")


class TestAddressees(unittest.TestCase):
    def _built(self, utts):
        chars = _chars()
        scenes = build_scenes(utts, chars)
        assign_addressees(utts, chars, scenes)
        return utts

    def test_vocative_head(self):
        utts = self._built([_u(0, "They stood in the city of Agrabah."),
                            _u(1, "Jafar, give me the lamp!", speaker="ch_a")])
        self.assertEqual(utts[1].addressee_id, "ch_j")

    def test_two_party_scene_inference(self):
        utts = self._built([_u(0, "They met in the city of Agrabah."),
                            _u(1, "Give it to me.", speaker="ch_a"),
                            _u(2, "Never.", speaker="ch_j")])
        self.assertEqual(utts[1].addressee_id, "ch_j")
        self.assertEqual(utts[2].addressee_id, "ch_a")

    def test_solo_speaker_stays_none(self):
        utts = self._built([_u(0, "Aladdin stood in the city of Agrabah."),
                            _u(1, "I wish to be a prince,", speaker="ch_a")])
        self.assertIsNone(utts[1].addressee_id)

    def test_vocative_never_self(self):
        utts = self._built([_u(0, "Aladdin was in the city of Agrabah."),
                            _u(1, "Aladdin, that is my name!", speaker="ch_a")])
        self.assertIsNone(utts[1].addressee_id)


class TestDossiers(unittest.TestCase):
    def test_rules_dossier_claims_only_evidence(self):
        chars = _chars()
        utts = [_u(0, "Aladdin met Jafar in the city of Agrabah."),
                _u(1, "Give me the lamp!", speaker="ch_j"),
                _u(2, "No!", speaker="ch_a")]
        scenes = build_scenes(utts, chars)
        summarize_scenes(scenes, utts)
        assign_addressees(utts, chars, scenes)

        class _Doc:  # dossier rules tier never reads the doc; LLM tier does
            chapters = []
        d = build_dossiers(_Doc(), chars, utts, scenes)["ch_a"]
        self.assertEqual(d.quotes, ["No!"])
        self.assertIn("speaks with", d.relationships["Jafar"])
        self.assertTrue(d.needs_review)
        self.assertEqual(d.generated_by, "rules")
        self.assertEqual(d.role, "")                      # rules tier never invents persona
        # spoiler gate: witnessed scene fact carries the scene's max_order
        self.assertEqual([(k.max_order) for k in d.knowledge], [2])


class TestLlmTierHardening(unittest.TestCase):
    def test_extract_json_survives_echoed_braces(self):
        from studio.knowledge.llm import extract_json
        reply = ("Based on the evidence {'Jafar': 'speaks with (3 exchange(s))'} here is "
                 'the card:\n{"role": "hero", "traits": ["brave"]}')
        self.assertEqual(extract_json(reply), {"role": "hero", "traits": ["brave"]})

    def test_extract_json_raises_without_object(self):
        from studio.knowledge.llm import extract_json
        with self.assertRaises(RuntimeError):
            extract_json("no json here { broken")

    def test_apply_llm_card_shape_checks(self):
        from studio.contract import Dossier
        from studio.knowledge.llm import apply_llm_card
        d = Dossier(character_id="ch_a", traits=["evidence"])
        # model violates the schema: traits is a string, relationships is a list
        apply_llm_card(d, {"role": "hero", "traits": "brave",
                           "relationships": ["not", "a", "dict"], "knowledge": "nope"}, [])
        self.assertEqual(d.role, "hero")
        self.assertEqual(d.traits, ["evidence"])       # bad type skipped, evidence kept
        self.assertEqual(d.relationships, {})
        self.assertEqual(d.generated_by, "llm")
        self.assertTrue(d.needs_review)

    def test_one_bad_character_does_not_strip_the_rest(self):
        import studio.knowledge.llm as llm_mod
        from studio.contract import Character, Dossier

        class _Doc:
            chapters = []
        chars = {"ch_a": Character(id="ch_a", canonical_name="A"),
                 "ch_b": Character(id="ch_b", canonical_name="B")}
        dossiers = {"ch_a": Dossier(character_id="ch_a"),
                    "ch_b": Dossier(character_id="ch_b")}
        calls = {"n": 0}

        def fake_ask(prompt):
            calls["n"] += 1
            if calls["n"] == 1:
                raise RuntimeError("boom")
            return {"role": "fine"}
        orig_ask, orig_cache = llm_mod._ask_claude, llm_mod._CACHE
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as tmp:
            llm_mod._ask_claude, llm_mod._CACHE = fake_ask, Path(tmp)
            try:
                llm_mod.enrich_dossiers(_Doc(), chars, dossiers, [], [])
            finally:
                llm_mod._ask_claude, llm_mod._CACHE = orig_ask, orig_cache
        self.assertEqual(dossiers["ch_a"].generated_by, "rules")   # failed, kept evidence
        self.assertEqual(dossiers["ch_b"].generated_by, "llm")     # unaffected
        self.assertEqual(dossiers["ch_b"].role, "fine")


class TestContractV02(unittest.TestCase):
    def test_scene_and_addressee_fks_validated(self):
        import json, tempfile
        from pathlib import Path
        from studio.build_book import build
        from studio.contract.validate import validate_bundle
        sample = str(Path(__file__).resolve().parents[1] / "samples" / "lighthouse.txt")
        with tempfile.TemporaryDirectory() as out:
            build(sample, out)
            self.assertEqual(validate_bundle(out), [])
            scenes = json.loads((Path(out) / "scenes.json").read_text())
            self.assertGreaterEqual(len(scenes), 1)
            utts = [json.loads(l) for l in (Path(out) / "utterances.jsonl").read_text().splitlines()]
            self.assertTrue(all(u["scene_id"] is not None for u in utts))
            # corrupt an FK → validator must catch it
            utts[0]["scene_id"] = "sc_bogus"
            (Path(out) / "utterances.jsonl").write_text(
                "\n".join(json.dumps(u) for u in utts) + "\n")
            self.assertTrue(any("sc_bogus" in e for e in validate_bundle(out)))


if __name__ == "__main__":
    unittest.main()
