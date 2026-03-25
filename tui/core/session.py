from dataclasses import dataclass, field
import os
from typing import List, Optional
from core.state_manager import UltraMode


@dataclass
class GovernanceConfig:
    require_plan_approval: bool = True
    require_action_approval: bool = False   # approve every tool call
    max_iterations: int = 10
    protected_paths: List[str] = field(default_factory=list)
    forbidden_actions: List[str] = field(default_factory=list)


@dataclass
class PolicyConfig:
    risk_tolerance: str = "medium"          # low | medium | high
    strategy_style: str = "balanced"        # fast | balanced | thorough
    auto_commit: bool = False
    run_tests_after_change: bool = True
    custom_rules: List[str] = field(default_factory=list)


class UltraSession:
    """Holds all user-configured settings for one Ultra session."""

    def __init__(self):
        self.mode: UltraMode = UltraMode.USER_DRIVEN
        self.project_root: str = "."
        self.governance: GovernanceConfig = GovernanceConfig()
        self.policy: PolicyConfig = PolicyConfig()
        self.project_context: Optional[str] = None   # architectural mode only

    def apply(self, config: dict) -> None:
        mode_value = config.get("mode", UltraMode.USER_DRIVEN)
        if isinstance(mode_value, UltraMode):
            self.mode = mode_value
        else:
            normalized_mode = str(mode_value).strip().lower()
            if normalized_mode == UltraMode.ARCHITECTURAL.value:
                self.mode = UltraMode.ARCHITECTURAL
            else:
                self.mode = UltraMode.USER_DRIVEN

        project_root = str(config.get("project_root", ".") or ".")
        self.project_root = os.path.abspath(os.path.expanduser(project_root))

        gov = config.get("governance", {})
        self.governance = GovernanceConfig(
            require_plan_approval=gov.get("require_plan_approval", True),
            require_action_approval=gov.get("require_action_approval", False),
            max_iterations=gov.get("max_iterations", 10),
            protected_paths=gov.get("protected_paths", []),
            forbidden_actions=gov.get("forbidden_actions", []),
        )

        pol = config.get("policy", {})
        self.policy = PolicyConfig(
            risk_tolerance=pol.get("risk_tolerance", "medium"),
            strategy_style=pol.get("strategy_style", "balanced"),
            auto_commit=pol.get("auto_commit", False),
            run_tests_after_change=pol.get("run_tests_after_change", True),
            custom_rules=pol.get("custom_rules", []),
        )

        self.project_context = config.get("project_context")

    def to_dict(self) -> dict:
        return {
            "mode": self.mode.value,
            "project_root": self.project_root,
            "governance": {
                "require_plan_approval": self.governance.require_plan_approval,
                "require_action_approval": self.governance.require_action_approval,
                "max_iterations": self.governance.max_iterations,
                "protected_paths": self.governance.protected_paths,
                "forbidden_actions": self.governance.forbidden_actions,
            },
            "policy": {
                "risk_tolerance": self.policy.risk_tolerance,
                "strategy_style": self.policy.strategy_style,
                "auto_commit": self.policy.auto_commit,
                "run_tests_after_change": self.policy.run_tests_after_change,
                "custom_rules": self.policy.custom_rules,
            },
        }

