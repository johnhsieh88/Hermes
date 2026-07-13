"""Stage 5 — Voice casting.  Deterministic, seeded character → voice-profile map.

Stable across chapters and across re-runs (the "stability is the whole game" invariant): the seed
and the library pick derive only from the character id via SHA-256 — no RNG, no wall-clock — so
regenerating the bundle yields the identical casting. A character keeps its voice; editing one
character re-renders only that character's lines (content-hash cache).
"""
from __future__ import annotations

import hashlib

from studio.contract import VoiceProfile, NARRATOR

# small fixed library of timbres per gender (Strategy A — fixed library, deterministic pick)
VOICE_LIB = {
    "female": ["af_warm", "af_bright", "af_calm", "af_low"],
    "male": ["am_deep", "am_warm", "am_bright", "am_low"],
    "unknown": ["nx_neutral_a", "nx_neutral_b"],
}
NARRATOR_VOICE = "nx_narrator"


def _seed(cid: str) -> int:
    return int(hashlib.sha256(cid.encode()).hexdigest()[:8], 16)


def build_casting(characters, engine: str = "xtts-v2"):
    """Returns (casting: {character_id: voice_profile_id}, profiles: {vp_id: VoiceProfile})."""
    casting, profiles = {}, {}

    # narrator profile always present (carries most of the runtime)
    profiles[NARRATOR_VOICE] = VoiceProfile(
        voice_profile_id=NARRATOR_VOICE, base_voice=NARRATOR_VOICE, engine=engine,
        seed=_seed(NARRATOR), style="narration")
    casting[NARRATOR] = NARRATOR_VOICE

    # per-gender round-robin over the library, ordered by prominence for stable picks
    used_per_gender = {}
    for c in sorted(characters, key=lambda x: (-x.mention_count, x.canonical_name)):
        lib = VOICE_LIB.get(c.gender, VOICE_LIB["unknown"])
        idx = used_per_gender.get(c.gender, 0)
        base = lib[idx % len(lib)]
        used_per_gender[c.gender] = idx + 1

        vp_id = f"vp_{c.id[3:]}"                       # stable id derived from the character id
        profiles[vp_id] = VoiceProfile(
            voice_profile_id=vp_id, base_voice=base, engine=engine,
            seed=_seed(c.id), style=c.gender)
        casting[c.id] = vp_id
        c.voice_profile_id = vp_id
    return casting, profiles
