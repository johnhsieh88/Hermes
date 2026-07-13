"""Validate a content bundle against the §3 contract.

The single gate both halves trust: the Studio runs it before publishing; Hermes (or CI) runs it
before ingesting. Usage:  python -m studio.contract.validate books/<id>
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

from studio.contract import CONTRACT_VERSION, EMOTIONS, NARRATOR, UNKNOWN

_REQUIRED_UTT = {"id", "chapter", "order", "text", "speaker_id", "emotion", "confidence"}


def validate_bundle(path) -> list:
    out = Path(path)
    errors = []

    def need(cond, msg):
        if not cond:
            errors.append(msg)

    mf_path = out / "manifest.json"
    if not mf_path.exists():
        return [f"missing manifest.json in {out}"]
    mf = json.loads(mf_path.read_text())

    need(mf.get("contract_version") == CONTRACT_VERSION,
         f"contract_version {mf.get('contract_version')!r} != {CONTRACT_VERSION!r}")

    # required sibling files
    for name in ("utterances.jsonl", "casting.json", "voice_profiles.json", "chapters.json",
                 "scenes.json"):
        need((out / name).exists(), f"missing {name}")

    casting = json.loads((out / "casting.json").read_text()) if (out / "casting.json").exists() else {}
    profiles = json.loads((out / "voice_profiles.json").read_text()) if (out / "voice_profiles.json").exists() else {}
    valid_speakers = set(casting.keys()) | {NARRATOR, UNKNOWN}

    # casting must reference real voice profiles
    for cid, vp in casting.items():
        need(vp in profiles, f"casting[{cid}] → unknown voice_profile {vp!r}")

    # scenes (v0.2): valid ranges, spoiler gates, participants are real characters
    scene_ids, char_ids = set(), {cid for cid in casting if cid != NARRATOR}
    spath = out / "scenes.json"
    if spath.exists():
        prev_end = -1
        for s in json.loads(spath.read_text(encoding="utf-8")):
            sid = s.get("scene_id", "?")
            scene_ids.add(sid)
            need(isinstance(s.get("order_start"), int) and isinstance(s.get("order_end"), int)
                 and s["order_start"] <= s["order_end"],
                 f"scene {sid}: bad order range")
            need(s.get("order_start", 0) > prev_end,
                 f"scene {sid}: overlaps/regresses previous scene")
            # a scene with a missing/bad order_end is already an error above; don't let it
            # silently weaken the overlap check for the scenes after it
            prev_end = s["order_end"] if isinstance(s.get("order_end"), int) else prev_end + 10**9
            need(s.get("max_order") == s.get("order_end"),
                 f"scene {sid}: max_order != order_end (spoiler gate broken)")
            for p in s.get("participants", []):
                need(p in char_ids, f"scene {sid}: participant {p!r} not a cast character")

    # utterances: schema, ordering, speaker validity, v0.2 FKs
    upath = out / "utterances.jsonl"
    if upath.exists():
        last_order = -1
        for i, line in enumerate(upath.read_text(encoding="utf-8").splitlines()):
            if not line.strip():
                continue
            u = json.loads(line)
            missing = _REQUIRED_UTT - u.keys()
            need(not missing, f"utterance {i}: missing fields {missing}")
            need(isinstance(u.get("order"), int) and u["order"] > last_order,
                 f"utterance {i}: order {u.get('order')} not globally monotonic")
            last_order = u.get("order", last_order)
            need(u.get("speaker_id") in valid_speakers,
                 f"utterance {i}: speaker_id {u.get('speaker_id')!r} not in casting/registry")
            emo = u.get("emotion", {})
            need(emo.get("label") in EMOTIONS, f"utterance {i}: emotion {emo.get('label')!r} not in vocab")
            need(0.0 <= float(u.get("confidence", -1)) <= 1.0, f"utterance {i}: confidence out of range")
            # audio (once rendered) must be keyed under audio/
            if u.get("audio"):
                need(str(u["audio"]).startswith("audio/"), f"utterance {i}: audio path not under audio/")
            # v0.2 nullable foreign keys
            if u.get("addressee_id") is not None:
                need(u["addressee_id"] in char_ids,
                     f"utterance {i}: addressee_id {u['addressee_id']!r} not a cast character")
            if u.get("scene_id") is not None:
                need(u["scene_id"] in scene_ids,
                     f"utterance {i}: scene_id {u['scene_id']!r} not in scenes.json")

    # integrity hashes present
    need(isinstance(mf.get("files"), dict) and mf["files"], "manifest.files integrity map is empty")
    return errors


def main():
    if len(sys.argv) != 2:
        print("usage: python -m studio.contract.validate <bundle_dir>")
        raise SystemExit(2)
    errs = validate_bundle(sys.argv[1])
    if errs:
        print(f"INVALID — {len(errs)} problem(s):")
        for e in errs:
            print("  •", e)
        raise SystemExit(1)
    print("VALID — bundle conforms to contract", CONTRACT_VERSION)


if __name__ == "__main__":
    main()
