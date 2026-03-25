from textual.widgets import Static
from textual.reactive import reactive
from core.state_manager import UltraMode


class ModeBar(Static):
    """One-line status bar below the header: mode + loop state."""

    mode_label: reactive[str] = reactive("USER-DRIVEN")
    state_label: reactive[str] = reactive("idle")

    def render(self) -> str:
        return f"  MODE: {self.mode_label}    STATE: {self.state_label}    ctrl+s = switch mode"

    def set_mode(self, mode: UltraMode) -> None:
        self.mode_label = "ARCHITECTURAL" if mode == UltraMode.ARCHITECTURAL else "USER-DRIVEN"

    def set_state(self, label: str) -> None:
        self.state_label = label
