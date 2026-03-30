"""Rotational animation frames and loop phase labels."""

THINKING_FRAMES = [
    "[ ∞ thinking ]",
    "[ ∞ expanding ]",
    "[ ∞ resolving ]",
]

LOOP_STATE_FRAMES = [
    "[ ◈ ]",
    "[ ◉ ]",
    "[ ◎ ]",
    "[ ◍ ]",
]

LOOP_PHASE_LABELS = {
    "INIT": "init",
    "PLAN": "plan",
    "ARBITRATION": "arbitration",
    "MICRO_PLAN": "micro-plan",
    "EXECUTE": "execute",
    "PARTIAL_REPAIR": "partial repair",
    "VERIFY": "verify",
    "REFLECT": "reflect",
    "RE_ANCHOR": "re-anchor",
    "REPLAN": "replan",
    "TERMINATE": "terminate",
}
