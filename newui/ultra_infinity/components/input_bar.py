"""
Input Bar Component Module

Fixed input bar at the bottom of the screen.
Accepts user input and triggers processing on submission.
Always visible and focused when idle.
"""

from textual.widgets import Input, Static
from textual.reactive import reactive
from textual.containers import Horizontal
from textual.message import Message


class InputBar(Horizontal):
    """
    Fixed input bar component for user input.
    
    Features:
    - Always visible at bottom
    - Accepts user input
    - Triggers processing on Enter
    - Clears after submission
    - Shows prompt indicator
    
    Emits InputSubmitted message when user presses Enter.
    """

    # Reactive properties
    is_enabled = reactive(True)
    placeholder_text = reactive("Type your message...")

    DEFAULT_CSS = """
    InputBar {
        width: 100%;
        height: 3;
        background: #0a0a0a;
        border-top: solid #1f1f1f;
        padding: 0 1;
    }
    
    InputBar Static.prompt {
        width: 2;
        content-align: center middle;
        color: #ffffff;
        text-style: bold;
        background: transparent;
    }
    
    InputBar Input {
        width: 1fr;
        height: 3;
        background: #0a0a0a;
        color: #ffffff;
        border: none;
        content-align: left middle;
        padding: 0 1;
    }
    
    InputBar Input:focus {
        border: none;
    }
    
    InputBar Input::placeholder {
        color: #666666;
        text-style: dim;
    }
    
    InputBar Input::cursor {
        background: #ffffff;
        color: #000000;
    }
    
    InputBar Input::selection {
        background: #333333;
        color: #ffffff;
    }
    """

    class InputSubmitted(Message):
        """
        Message emitted when user submits input.
        
        Attributes:
            value: The submitted input text.
        """
        
        def __init__(self, value: str) -> None:
            """
            Initialize the message.
            
            Args:
                value: The submitted input value.
            """
            super().__init__()
            self.value = value

    def __init__(self):
        """Initialize the input bar."""
        super().__init__()
        self._input_field: Input | None = None
        self._prompt: Static | None = None

    def compose(self):
        """Compose the input bar layout."""
        # Prompt indicator
        self._prompt = Static(">", id="input-prompt", classes="prompt")
        yield self._prompt
        
        # Input field
        self._input_field = Input(
            placeholder=self.placeholder_text,
            id="input-field"
        )
        yield self._input_field

    def on_mount(self) -> None:
        """Called when the component is mounted."""
        self._input_field = self.query_one("#input-field", Input)
        self._prompt = self.query_one("#input-prompt", Static)
        
        # Focus the input field
        self.focus_input()

    def on_input_submitted(self, event: Input.Submitted) -> None:
        """
        Handle input submission.
        
        Args:
            event: The submitted event from the Input widget.
        """
        if not self.is_enabled:
            return
        
        value = event.value.strip()
        if value:
            # Emit our custom message
            self.post_message(self.InputSubmitted(value))
            
            # Clear the input
            self.clear_input()

    def focus_input(self) -> None:
        """Focus the input field."""
        if self._input_field:
            self._input_field.focus()

    def clear_input(self) -> None:
        """Clear the input field."""
        if self._input_field:
            self._input_field.value = ""

    def set_enabled(self, enabled: bool) -> None:
        """
        Enable or disable the input bar.
        
        Args:
            enabled: True to enable, False to disable.
        """
        self.is_enabled = enabled
        if self._input_field:
            self._input_field.disabled = not enabled
            if enabled:
                self.focus_input()

    def set_placeholder(self, text: str) -> None:
        """
        Set the placeholder text.
        
        Args:
            text: The new placeholder text.
        """
        self.placeholder_text = text
        if self._input_field:
            self._input_field.placeholder = text

    def get_value(self) -> str:
        """
        Get the current input value.
        
        Returns:
            The current input text.
        """
        if self._input_field:
            return self._input_field.value
        return ""

    def set_value(self, value: str) -> None:
        """
        Set the input value.
        
        Args:
            value: The value to set.
        """
        if self._input_field:
            self._input_field.value = value

    def watch_is_enabled(self, enabled: bool) -> None:
        """
        React to enabled state changes.
        
        Args:
            enabled: The new enabled state.
        """
        if self._input_field:
            self._input_field.disabled = not enabled
            if enabled:
                self.focus_input()

    def watch_placeholder_text(self, text: str) -> None:
        """
        React to placeholder text changes.
        
        Args:
            text: The new placeholder text.
        """
        if self._input_field:
            self._input_field.placeholder = text
