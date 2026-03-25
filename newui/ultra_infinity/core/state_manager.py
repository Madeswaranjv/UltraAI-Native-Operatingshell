"""
State Manager Module

Manages the application state transitions for the Ultra Infinity CLI.
Handles states: IDLE, THINKING, STREAMING, COMPLETE

Provides clean state transitions and event callbacks for state changes.
"""

from enum import Enum, auto
from typing import Optional, Callable, List
from dataclasses import dataclass, field


class AppState(Enum):
    """
    Application state enumeration.
    
    States:
        IDLE: Waiting for user input
        THINKING: AI is processing the request
        STREAMING: AI is generating response
        COMPLETE: Response generation finished
    """
    IDLE = auto()
    THINKING = auto()
    STREAMING = auto()
    COMPLETE = auto()


@dataclass
class StateTransition:
    """Represents a state transition event."""
    from_state: AppState
    to_state: AppState
    timestamp: float = field(default_factory=lambda: __import__('time').time())


class StateManager:
    """
    Manages application state with clean transitions and event handling.
    
    This is a singleton-style manager that tracks the current state
    and notifies registered callbacks of state changes.
    """

    def __init__(self):
        self._current_state: AppState = AppState.IDLE
        self._previous_state: Optional[AppState] = None
        self._transition_history: List[StateTransition] = []
        self._callbacks: dict[AppState, List[Callable[[], None]]] = {
            state: [] for state in AppState
        }
        self._any_state_callbacks: List[Callable[[AppState, AppState], None]] = []

    @property
    def current_state(self) -> AppState:
        """Get the current application state."""
        return self._current_state

    @property
    def previous_state(self) -> Optional[AppState]:
        """Get the previous application state."""
        return self._previous_state

    @property
    def is_idle(self) -> bool:
        """Check if currently in IDLE state."""
        return self._current_state == AppState.IDLE

    @property
    def is_thinking(self) -> bool:
        """Check if currently in THINKING state."""
        return self._current_state == AppState.THINKING

    @property
    def is_streaming(self) -> bool:
        """Check if currently in STREAMING state."""
        return self._current_state == AppState.STREAMING

    @property
    def is_complete(self) -> bool:
        """Check if currently in COMPLETE state."""
        return self._current_state == AppState.COMPLETE

    @property
    def is_processing(self) -> bool:
        """Check if currently processing (thinking or streaming)."""
        return self._current_state in (AppState.THINKING, AppState.STREAMING)

    def transition_to(self, new_state: AppState) -> None:
        """
        Transition to a new state.
        
        Args:
            new_state: The state to transition to.
            
        Raises:
            ValueError: If the transition is invalid.
        """
        if new_state == self._current_state:
            return

        # Validate transition
        if not self._is_valid_transition(self._current_state, new_state):
            raise ValueError(
                f"Invalid state transition: {self._current_state.name} -> {new_state.name}"
            )

        # Record transition
        transition = StateTransition(
            from_state=self._current_state,
            to_state=new_state
        )
        self._transition_history.append(transition)

        # Update states
        self._previous_state = self._current_state
        self._current_state = new_state

        # Notify callbacks
        self._notify_state_change(self._previous_state, new_state)

    def _is_valid_transition(self, from_state: AppState, to_state: AppState) -> bool:
        """
        Check if a state transition is valid.
        
        Valid transitions:
            IDLE -> THINKING
            THINKING -> STREAMING
            STREAMING -> COMPLETE
            COMPLETE -> IDLE
            ANY -> IDLE (for cancellation)
        """
        valid_transitions = {
            AppState.IDLE: [AppState.THINKING],
            AppState.THINKING: [AppState.STREAMING, AppState.IDLE],
            AppState.STREAMING: [AppState.COMPLETE, AppState.IDLE],
            AppState.COMPLETE: [AppState.IDLE],
        }
        return to_state in valid_transitions.get(from_state, [])

    def _notify_state_change(
        self, from_state: AppState, to_state: AppState
    ) -> None:
        """Notify all registered callbacks of a state change."""
        # Notify specific state callbacks
        for callback in self._callbacks.get(to_state, []):
            try:
                callback()
            except Exception:
                pass

        # Notify any-state callbacks
        for callback in self._any_state_callbacks:
            try:
                callback(from_state, to_state)
            except Exception:
                pass

    def on_state_enter(
        self, state: AppState, callback: Callable[[], None]
    ) -> None:
        """
        Register a callback to be called when entering a state.
        
        Args:
            state: The state to watch.
            callback: Function to call when entering the state.
        """
        if state not in self._callbacks:
            self._callbacks[state] = []
        self._callbacks[state].append(callback)

    def on_any_state_change(
        self, callback: Callable[[AppState, AppState], None]
    ) -> None:
        """
        Register a callback for any state change.
        
        Args:
            callback: Function(from_state, to_state) to call on any transition.
        """
        self._any_state_callbacks.append(callback)

    def reset(self) -> None:
        """Reset to IDLE state and clear history."""
        self._previous_state = None
        self._current_state = AppState.IDLE
        self._transition_history.clear()

    def get_transition_history(self) -> List[StateTransition]:
        """Get the history of state transitions."""
        return self._transition_history.copy()

    def __str__(self) -> str:
        return f"StateManager(current={self._current_state.name})"

    def __repr__(self) -> str:
        return (
            f"StateManager("
            f"current={self._current_state.name}, "
            f"previous={self._previous_state.name if self._previous_state else None}"
            f")"
        )


# Global state manager instance
_state_manager: Optional[StateManager] = None


def get_state_manager() -> StateManager:
    """Get the global state manager instance (singleton)."""
    global _state_manager
    if _state_manager is None:
        _state_manager = StateManager()
    return _state_manager


def reset_state_manager() -> None:
    """Reset the global state manager."""
    global _state_manager
    _state_manager = StateManager()
