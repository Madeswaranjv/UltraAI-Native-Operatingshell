"""Anime-styled phrases used by the live cognitive stream."""

ANIME_WORDS = [
    "Bankai",
    "Domain Expansion",
    "Hollow Purple",
    "Gear Five",
    "Gear Second",
    "Gear Four",
    "Ashura Mode",
    "Rinnegan Activation",
    "Mangekyo Sharingan",
    "Eternal Mangekyo",
    "Six Paths Mode",
    "Rasengan",
    "Chidori",
    "Getsuga Tensho",
    "Mugetsu",
    "Black Flash",
    "Reverse Cursed Technique",
    "Malevolent Shrine",
    "Infinity Void",
    "Limitless",
    "Blue Technique",
    "Red Technique",
    "Soul Reaper Form",
    "Quincy Vollstandig",
    "Zangetsu Release",
    "Fire Breathing",
    "Water Breathing",
    "Thunder Breathing",
    "Sun Breathing",
    "Beast Breathing",
    "Spirit Gun",
    "Full Counter",
    "Overdrive",
    "Hyper Mode",
    "Final Getsuga",
    "God like speed",
    "Lightning Cloak",
    "Shadow Clone Jutsu",
    "Truth Seeking Orbs",
    "Meteor Strike",
    "Void Collapse",
    "Energy Convergence",
    "Quantum Break",
    "Infinity Pulse",
    "System Override",
    "Ultra Sync",
]

DEFAULT_STREAM_WORDS = [
    "Analyzing...",
    "Kakugo...",
    "Shinka...",
    "Processing Intent...",
    "Resolving Fate...",
]

STATE_STREAM_WORDS = {
    "INIT": [
        "Analyzing...",
        "Calibrating...",
        "Kakugo...",
    ],
    "PLAN": [
        "Strategizing...",
        "Processing Intent...",
        "Charting Outcome...",
    ],
    "ARBITRATION": [
        "Choosing Path...",
        "Resolving Conflict...",
        "Fate Selection...",
    ],
    "MICRO_PLAN": [
        "Breaking It Down...",
        "Thread Weaving...",
        "Shinka...",
    ],
    "EXECUTE": [
        "Unleashing...",
        "Committing Force...",
        "Overdrive...",
    ],
    "PARTIAL_REPAIR": [
        "Repairing Flow...",
        "Mending Threads...",
        "Stabilizing...",
    ],
    "VERIFY": [
        "Validating Reality...",
        "Proofing Outcome...",
        "Truth Scan...",
    ],
    "REFLECT": [
        "Recalibrating...",
        "Echo Review...",
        "Internalizing...",
    ],
    "RE_ANCHOR": [
        "Re-anchoring...",
        "Realigning Intent...",
        "Returning Course...",
    ],
    "REPLAN": [
        "Rewriting Destiny...",
        "Resetting Vectors...",
        "Second Pass...",
    ],
    "TERMINATE": [
        "Stabilized...",
        "Resolved...",
        "Sealing Outcome...",
    ],
}


def anime_words_for_phase(phase: str | None) -> list[str]:
    normalized = str(phase or "").upper()
    phase_words = list(STATE_STREAM_WORDS.get(normalized, DEFAULT_STREAM_WORDS))
    generic_words = [
        phrase if phrase.endswith("...") else f"{phrase}..."
        for phrase in ANIME_WORDS
    ]
    return phase_words + generic_words
