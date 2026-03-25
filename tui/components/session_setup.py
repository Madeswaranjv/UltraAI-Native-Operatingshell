"""
SessionSetup — the first screen the user sees.

Walks through:
  1. Mode selection  (Architectural | User-Driven)
  2. Project root
  3. Governance settings
  4. Policy / risk settings
  5. Confirmation → fires SessionSetup.Completed message

All keyboard-driven, zero mouse required.
"""
from __future__ import annotations
from textual.app import ComposeResult
from textual.containers import Container, Vertical
from textual.widgets import Static, Input, Select
from textual.message import Message
from textual.binding import Binding

from core.state_manager import UltraMode


STEPS = [
    "mode",
    "project_root",
    "risk_tolerance",
    "strategy_style",
    "protected_paths",
    "require_plan_approval",
    "confirm",
]

STEP_PROMPTS = {
    "mode":                  "SELECT MODE  [ 1 ] Architectural   [ 2 ] User-Driven",
    "project_root":          "PROJECT ROOT  (path, or press Enter for current directory)",
    "risk_tolerance":        "RISK TOLERANCE  [ 1 ] Low   [ 2 ] Medium   [ 3 ] High",
    "strategy_style":        "STRATEGY  [ 1 ] Fast   [ 2 ] Balanced   [ 3 ] Thorough",
    "protected_paths":       "PROTECTED PATHS  (comma-separated, or Enter to skip)",
    "require_plan_approval": "REQUIRE PLAN APPROVAL?  [ Y ] Yes   [ N ] No  (default: Y)",
    "confirm":               "READY  —  press Enter to initialise Ultra",
}


class SessionSetup(Container):

    class Completed(Message):
        def __init__(self, config: dict):
            super().__init__()
            self.config = config

    BINDINGS = [Binding("enter", "submit", "Submit", show=False)]

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self._step_index = 0
        self._answers: dict = {}

    def compose(self) -> ComposeResult:
        yield Vertical(
            Static("", id="setup-banner"),
            Static("", id="setup-prompt"),
            Static("", id="setup-hint"),
            Input(placeholder="", id="setup-input"),
            Static("", id="setup-summary"),
            id="setup-inner",
        )

    def on_mount(self) -> None:
        self._render_step()
        self.query_one("#setup-input").focus()

    def on_input_submitted(self, message) -> None:
        value = message.input.value.strip()
        message.input.value = ""
        self._process_answer(value)

    def _current_step(self) -> str:
        return STEPS[self._step_index]

    def _render_step(self) -> None:
        step = self._current_step()
        total = len(STEPS)
        progress = f"  [{self._step_index + 1}/{total}]"

        self.query_one("#setup-banner").update(
            f"ULTRA ∞  —  SESSION INITIALISATION{progress}"
        )
        self.query_one("#setup-prompt").update(STEP_PROMPTS[step])
        self.query_one("#setup-hint").update(self._hint(step))

        if step == "confirm":
            self.query_one("#setup-summary").update(self._build_summary())
            self.query_one("#setup-input").placeholder = "Press Enter to start..."
        else:
            self.query_one("#setup-summary").update("")

    def _hint(self, step: str) -> str:
        hints = {
            "mode":                  "Architectural: Ultra plans autonomously | User-Driven: you prompt per task",
            "project_root":          "Ultra will analyse this directory with ultra analyze",
            "risk_tolerance":        "Low = conservative, needs approval | High = fast, fewer gates",
            "strategy_style":        "Fast = minimal steps | Thorough = deep verification",
            "protected_paths":       "e.g.  src/auth, payments  — Ultra will never modify these",
            "require_plan_approval": "If Y, Ultra shows the full plan and waits for your approval",
            "confirm":               "Review your settings above. Enter to launch.",
        }
        return hints.get(step, "")

    def _process_answer(self, value: str) -> None:
        step = self._current_step()

        if step == "mode":
            if value in ("1", "architectural", "a"):
                self._answers["mode"] = UltraMode.ARCHITECTURAL
            else:
                self._answers["mode"] = UltraMode.USER_DRIVEN

        elif step == "project_root":
            self._answers["project_root"] = value if value else "."

        elif step == "risk_tolerance":
            mapping = {"1": "low", "2": "medium", "3": "high",
                       "low": "low", "medium": "medium", "high": "high"}
            self._answers["risk_tolerance"] = mapping.get(value.lower(), "medium")

        elif step == "strategy_style":
            mapping = {"1": "fast", "2": "balanced", "3": "thorough",
                       "fast": "fast", "balanced": "balanced", "thorough": "thorough"}
            self._answers["strategy_style"] = mapping.get(value.lower(), "balanced")

        elif step == "protected_paths":
            paths = [p.strip() for p in value.split(",") if p.strip()]
            self._answers["protected_paths"] = paths

        elif step == "require_plan_approval":
            self._answers["require_plan_approval"] = value.lower() not in ("n", "no")

        elif step == "confirm":
            self._emit_completed()
            return

        self._step_index += 1
        self._render_step()

    def _build_summary(self) -> str:
        a = self._answers
        mode_label = "Architectural" if a.get("mode") == UltraMode.ARCHITECTURAL else "User-Driven"
        paths = ", ".join(a.get("protected_paths", [])) or "none"
        approval = "yes" if a.get("require_plan_approval", True) else "no"
        lines = [
            "",
            f"  mode              →  {mode_label}",
            f"  project root      →  {a.get('project_root', '.')}",
            f"  risk tolerance    →  {a.get('risk_tolerance', 'medium')}",
            f"  strategy          →  {a.get('strategy_style', 'balanced')}",
            f"  protected paths   →  {paths}",
            f"  plan approval     →  {approval}",
            "",
        ]
        return "\n".join(lines)

    def _emit_completed(self) -> None:
        a = self._answers
        config = {
            "mode": a.get("mode", UltraMode.USER_DRIVEN),
            "project_root": a.get("project_root", "."),
            "governance": {
                "require_plan_approval": a.get("require_plan_approval", True),
                "require_action_approval": False,
                "max_iterations": 10,
                "protected_paths": a.get("protected_paths", []),
                "forbidden_actions": [],
            },
            "policy": {
                "risk_tolerance": a.get("risk_tolerance", "medium"),
                "strategy_style": a.get("strategy_style", "balanced"),
                "auto_commit": False,
                "run_tests_after_change": True,
                "custom_rules": [],
            },
        }
        self.post_message(self.Completed(config))
