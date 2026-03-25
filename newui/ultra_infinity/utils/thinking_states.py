"""
Thinking States Module

Manages the cycling thinking indicators displayed during AI processing.
Provides smooth state transitions and updates for the thinking animation.
"""

from typing import List
from enum import Enum, auto


class ThinkingStateType(Enum):
    """Enumeration of possible thinking state types."""
    PROCESSING = auto()
    ANALYZING = auto()
    REASONING = auto()
    EXECUTING = auto()


class ThinkingState:
    """
    Represents a single thinking state with its display text.
    """

    def __init__(self, state_type: ThinkingStateType, text: str):
        self.state_type = state_type
        self.text = text

    def __str__(self) -> str:
        return self.text

    def __repr__(self) -> str:
        return f"ThinkingState({self.state_type.name}: {self.text})"


# Classic thinking states cycle
CLASSIC_STATES: List[ThinkingState] = [
    ThinkingState(ThinkingStateType.PROCESSING, "[ Processing... ]"),
    ThinkingState(ThinkingStateType.ANALYZING, "[ Analyzing... ]"),
    ThinkingState(ThinkingStateType.REASONING, "[ Reasoning... ]"),
    ThinkingState(ThinkingStateType.EXECUTING, "[ Executing... ]"),
]

# Infinity-themed thinking states cycle
INFINITY_STATES: List[ThinkingState] = [
    ThinkingState(ThinkingStateType.PROCESSING, "[ ∞ thinking ]"),
    ThinkingState(ThinkingStateType.ANALYZING, "[ ∞ expanding ]"),
    ThinkingState(ThinkingStateType.REASONING, "[ ∞ resolving ]"),
    ThinkingState(ThinkingStateType.EXECUTING, "[ ∞ converging ]"),
]


class ThinkingStateCycler:
    """
    Cycles through thinking states for the thinking animation.
    Updates every ~200ms with smooth text transitions.
    """

    def __init__(self, use_infinity: bool = True):
        """
        Initialize the thinking state cycler.
        
        Args:
            use_infinity: If True, use infinity-themed states; otherwise use classic.
        """
        self._states = INFINITY_STATES if use_infinity else CLASSIC_STATES
        self._current_index = 0
        self._use_infinity = use_infinity

    def get_current(self) -> ThinkingState:
        """
        Get the current thinking state.
        
        Returns:
            ThinkingState: The current state.
        """
        return self._states[self._current_index]

    def get_current_text(self) -> str:
        """
        Get the current thinking state text.
        
        Returns:
            str: The display text for the current state.
        """
        return self._states[self._current_index].text

    def next(self) -> ThinkingState:
        """
        Advance to the next thinking state and return it.
        
        Returns:
            ThinkingState: The next state in the cycle.
        """
        self._current_index = (self._current_index + 1) % len(self._states)
        return self._states[self._current_index]

    def next_text(self) -> str:
        """
        Advance to the next state and return its text.
        
        Returns:
            str: The display text for the next state.
        """
        return self.next().text

    def reset(self) -> None:
        """Reset the cycler to the initial state."""
        self._current_index = 0

    def set_infinity_mode(self, use_infinity: bool) -> None:
        """
        Switch between infinity and classic modes.
        
        Args:
            use_infinity: True for infinity mode, False for classic.
        """
        self._states = INFINITY_STATES if use_infinity else CLASSIC_STATES
        self._use_infinity = use_infinity
        self.reset()

    @property
    def state_count(self) -> int:
        """Return the number of states in the cycle."""
        return len(self._states)

    @property
    def is_infinity_mode(self) -> bool:
        """Check if currently using infinity-themed states."""
        return self._use_infinity


# Global cycler instance for shared state
_default_cycler = ThinkingStateCycler(use_infinity=True)


def get_thinking_text() -> str:
    """Get the current thinking state text."""
    return _default_cycler.get_current_text()


def next_thinking_text() -> str:
    """Advance and get the next thinking state text."""
    return _default_cycler.next_text()


def reset_thinking_states() -> None:
    """Reset the thinking state cycler."""
    _default_cycler.reset()
