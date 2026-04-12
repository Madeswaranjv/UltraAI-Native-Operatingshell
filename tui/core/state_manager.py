from enum import Enum, auto


class AppState(Enum):
    IDLE = auto()
    THINKING = auto()
    INITIALIZING = auto()
    PARSING_INTENT = auto()
    PLANNING = auto()
    ARBITRATING = auto()
    MICRO_PLANNING = auto()
    GOVERNANCE_CHECK = auto()
    EXECUTING = auto()
    REPAIRING = auto()
    VERIFYING = auto()
    REFLECTING = auto()
    REANCHORING = auto()
    REPLANNING = auto()
    COMPLETE = auto()
    ERROR = auto()


class UltraMode(Enum):
    ARCHITECTURAL = "architectural"
    USER_DRIVEN = "user_driven"


# Human-readable labels shown in the UI for each state
STATE_LABELS = {
    AppState.IDLE:             "idle",
    AppState.THINKING:         "thinking",
    AppState.INITIALIZING:     "initializing",
    AppState.PARSING_INTENT:   "parsing intent",
    AppState.PLANNING:         "planning",
    AppState.ARBITRATING:      "arbitrating",
    AppState.MICRO_PLANNING:   "micro-planning",
    AppState.GOVERNANCE_CHECK: "governance check",
    AppState.EXECUTING:        "executing",
    AppState.REPAIRING:        "repairing",
    AppState.VERIFYING:        "verifying",
    AppState.REFLECTING:       "reflecting",
    AppState.REANCHORING:      "re-anchoring",
    AppState.REPLANNING:       "replanning",
    AppState.COMPLETE:         "complete",
    AppState.ERROR:            "error",
}


class StateManager:
    def __init__(self):
        self.current_state = AppState.IDLE
        self.current_mode = UltraMode.USER_DRIVEN
        self.verbose_stream = False
        self._listeners = []

    def set_state(self, state: AppState) -> None:
        if self.current_state == state:
            return
        self.current_state = state
        for cb in self._listeners:
            cb(state)

    def set_mode(self, mode: UltraMode) -> None:
        self.current_mode = mode

    def set_verbose_stream(self, enabled: bool) -> None:
        self.verbose_stream = enabled

    def add_listener(self, callback) -> None:
        self._listeners.append(callback)

    @property
    def state_label(self) -> str:
        return STATE_LABELS.get(self.current_state, "unknown")
