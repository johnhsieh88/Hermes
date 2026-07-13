"""Shared heuristic lexicons for the rules tiers (attribution + emotion).

Deliberately small and readable — this is the cheap tier the PDF says to run first. The ML/LLM
tiers (spaCy, fine-tuned classifier, LLM sliding-window) refine what these can't resolve.
"""

# Dialogue verbs that mark an explicit attribution ("said X" / "X replied").
SAY_VERBS = {
    "said", "says", "replied", "asked", "answered", "cried", "whispered", "shouted",
    "muttered", "murmured", "exclaimed", "added", "continued", "began", "sighed",
    "laughed", "gasped", "snapped", "called", "returned", "spoke", "breathed",
    "groaned", "sobbed", "roared", "hissed", "pleaded", "insisted", "declared",
}

# Cue word → (7-state emotion, base intensity). Intensity is capped conservatively (PDF Stage 4:
# "slightly-too-flat reads far better than caricature"). Used on the narration around a quote.
EMOTION_CUES = {
    # anger
    "snapped": ("anger", 0.6), "shouted": ("anger", 0.6), "roared": ("anger", 0.7),
    "hissed": ("anger", 0.5), "growled": ("anger", 0.5),
    # sadness
    "sobbed": ("sadness", 0.6), "wept": ("sadness", 0.6), "sighed": ("sadness", 0.35),
    "groaned": ("sadness", 0.4), "mourned": ("sadness", 0.6),
    # fear
    "gasped": ("fear", 0.5), "trembled": ("fear", 0.5), "stammered": ("fear", 0.45),
    "whispered": ("fear", 0.3),
    # joy
    "laughed": ("joy", 0.5), "grinned": ("joy", 0.45), "chuckled": ("joy", 0.4),
    "joyfully": ("joy", 0.6), "smiled": ("joy", 0.35),
    # surprise
    "exclaimed": ("surprise", 0.5), "blurted": ("surprise", 0.5),
    # tenderness
    "murmured": ("tenderness", 0.35), "soothed": ("tenderness", 0.5),
    "softly": ("tenderness", 0.3), "gently": ("tenderness", 0.35),
}

PRONOUN_GENDER = {
    "he": "male", "him": "male", "his": "male",
    "she": "female", "her": "female", "hers": "female",
}

MALE_HONORIFICS = {"mr", "sir", "lord", "master", "king", "prince", "father", "brother",
                   "sultan", "emperor"}
FEMALE_HONORIFICS = {"mrs", "ms", "miss", "lady", "madam", "queen", "princess", "mother",
                     "sister", "sultana", "empress"}
