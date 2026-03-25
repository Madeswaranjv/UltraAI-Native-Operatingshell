"""
IntentParser — converts freeform user text into a structured IntentPayload
that the C++ CognitiveRuntime / IntentRuntime can consume.

Current implementation: uses keyword/pattern heuristics so the TUI works
without a live model.  Replace `_call_model()` with a real Ollama/OpenAI
call once the backend bridge is wired.
"""
from __future__ import annotations
import re
import asyncio
from dataclasses import dataclass, field
from typing import List, Optional


ACTION_KEYWORDS = {
    "add":      ["add", "create", "implement", "build", "introduce", "write"],
    "fix":      ["fix", "repair", "debug", "resolve", "patch", "correct"],
    "refactor": ["refactor", "restructure", "reorganise", "reorganize", "clean", "simplify"],
    "remove":   ["remove", "delete", "drop", "eliminate"],
    "test":     ["test", "spec", "unit test", "integration test", "coverage"],
    "analyse":  ["analyse", "analyze", "explain", "understand", "describe", "what is"],
    "optimise": ["optimise", "optimize", "speed up", "performance", "faster"],
}

RISK_MAP = {
    "low":    ["explain", "analyse", "analyze", "describe", "what is", "show"],
    "high":   ["delete", "remove", "drop", "refactor", "restructure", "rewrite"],
}


@dataclass
class IntentPayload:
    """Structured intent — maps directly to Ultra's IntentRuntime input format."""
    raw_prompt: str
    action: str                         # add | fix | refactor | remove | test | analyse | optimise
    targets: List[str]                  # symbol / file / module names mentioned
    goal_summary: str                   # one-sentence restatement
    constraints: List[str]              # extracted constraints ("don't break the API")
    risk_level: str                     # low | medium | high
    requires_planning: bool             # true for multi-step tasks
    context_hint: Optional[str] = None  # extra context to seed the graph query

    def to_dict(self) -> dict:
        return {
            "raw_prompt": self.raw_prompt,
            "action": self.action,
            "targets": self.targets,
            "goal_summary": self.goal_summary,
            "constraints": self.constraints,
            "risk_level": self.risk_level,
            "requires_planning": self.requires_planning,
            "context_hint": self.context_hint,
        }


class IntentParser:
    """Parse a freeform prompt into a structured IntentPayload."""

    async def parse(self, prompt: str, session=None) -> IntentPayload:
        # Simulate async work — replace with real model call later
        await asyncio.sleep(0.05)
        return self._heuristic_parse(prompt, session)

    # ── Internal ────────────────────────────────────────────────────────────

    def _heuristic_parse(self, prompt: str, session) -> IntentPayload:
        lower = prompt.lower()

        action = self._detect_action(lower)
        targets = self._extract_targets(prompt)
        risk = self._detect_risk(lower, action)
        constraints = self._extract_constraints(lower)
        requires_planning = action in ("add", "refactor", "fix", "optimise")
        goal_summary = self._build_goal_summary(action, targets, prompt)

        return IntentPayload(
            raw_prompt=prompt,
            action=action,
            targets=targets,
            goal_summary=goal_summary,
            constraints=constraints,
            risk_level=risk,
            requires_planning=requires_planning,
            context_hint=targets[0] if targets else None,
        )

    def _detect_action(self, lower: str) -> str:
        for action, keywords in ACTION_KEYWORDS.items():
            if any(k in lower for k in keywords):
                return action
        return "add"

    def _extract_targets(self, prompt: str) -> List[str]:
        # PascalCase / camelCase identifiers and quoted strings
        identifiers = re.findall(r"\b[A-Z][a-zA-Z0-9]{2,}\b", prompt)
        quoted = re.findall(r'["\']([^"\']+)["\']', prompt)
        # path-like tokens
        paths = re.findall(r"\b[\w./\\]+\.[a-zA-Z]{1,6}\b", prompt)
        seen, result = set(), []
        for t in identifiers + quoted + paths:
            if t not in seen:
                seen.add(t)
                result.append(t)
        return result[:5]  # cap at 5

    def _detect_risk(self, lower: str, action: str) -> str:
        for level, keywords in RISK_MAP.items():
            if any(k in lower for k in keywords):
                return level
        return "medium"

    def _extract_constraints(self, lower: str) -> List[str]:
        patterns = [
            r"don['\u2019]?t (.+?)(?:\.|,|$)",
            r"without (.+?)(?:\.|,|$)",
            r"must (.+?)(?:\.|,|$)",
            r"keep (.+?)(?:\.|,|$)",
            r"maintain (.+?)(?:\.|,|$)",
        ]
        results = []
        for p in patterns:
            for m in re.finditer(p, lower):
                results.append(m.group(0).strip())
        return results[:4]

    def _build_goal_summary(self, action: str, targets: List[str], prompt: str) -> str:
        if targets:
            return f"{action.capitalize()} {', '.join(targets[:2])}"
        # fallback: trim to ~60 chars
        return prompt[:60].strip() + ("…" if len(prompt) > 60 else "")


# ── Convenience ──────────────────────────────────────────────────────────────

async def parse_intent(prompt: str, session=None) -> IntentPayload:
    return await IntentParser().parse(prompt, session)
