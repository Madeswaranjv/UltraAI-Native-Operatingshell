"""
Output Panel Component Module

Scrollable output area for displaying AI responses, thinking animations,
and streaming content. This is the most important component of the UI.
"""

from textual.widgets import RichLog, Static
from textual.reactive import reactive
from textual.containers import Vertical
from textual.color import Color


class OutputPanel(Vertical):
    """
    Scrollable output panel for AI responses and streaming content.
    
    Features:
    - Scrollable content area
    - Appending text support
    - Updating last line support
    - Streaming content support
    - Auto-scroll to follow content
    - Clean line spacing
    
    This is the primary display area for all AI output.
    """

    # Reactive properties
    current_thinking = reactive("")
    is_thinking = reactive(False)

    DEFAULT_CSS = """
    OutputPanel {
        width: 100%;
        height: 1fr;
        background: #000000;
        border: none;
        padding: 0;
    }
    
    OutputPanel RichLog {
        width: 100%;
        height: 100%;
        background: #000000;
        color: #e6e6e6;
        border: none;
        padding: 1 2;
        scrollbar-size: 1 1;
        scrollbar-background: #000000;
        scrollbar-color: #1f1f1f;
    }
    
    OutputPanel Static.thinking-indicator {
        width: 100%;
        height: 1;
        background: #000000;
        color: #666666;
        content-align: left middle;
        text-style: dim;
    }
    """

    def __init__(self):
        """Initialize the output panel."""
        super().__init__()
        self._content_lines: list[str] = []
        self._current_line: str = ""
        self._thinking_indicator: Static | None = None
        self._output_log: RichLog | None = None

    def compose(self):
        """Compose the output panel layout."""
        # Thinking indicator (hidden by default)
        self._thinking_indicator = Static(
            "",
            id="thinking-indicator",
            classes="thinking-indicator"
        )
        self._thinking_indicator.visible = False
        yield self._thinking_indicator
        
        # Main output log
        self._output_log = RichLog(
            id="output-log",
            wrap=True,
            highlight=True,
            markup=True
        )
        yield self._output_log

    def on_mount(self) -> None:
        """Called when the component is mounted."""
        self._thinking_indicator = self.query_one("#thinking-indicator", Static)
        self._output_log = self.query_one("#output-log", RichLog)

    def append_text(self, text: str, scroll: bool = True) -> None:
        """
        Append text to the output.
        
        Args:
            text: The text to append.
            scroll: Whether to auto-scroll to the new content.
        """
        if self._output_log:
            self._output_log.write(text)
            if scroll:
                self._output_log.scroll_end(animate=False)

    def append_line(self, line: str, scroll: bool = True) -> None:
        """
        Append a line to the output (adds newline).
        
        Args:
            line: The line to append.
            scroll: Whether to auto-scroll to the new content.
        """
        self.append_text(line + "\n", scroll)

    def update_last_line(self, text: str) -> None:
        """
        Update the last line of output.
        
        Note: RichLog doesn't support line updates directly,
        so we clear and rewrite, or append a correction.
        
        Args:
            text: The new text for the last line.
        """
        # For now, just append the update
        # In a more complex implementation, we could track lines
        self.append_text(text)

    def clear_output(self) -> None:
        """Clear all output content."""
        if self._output_log:
            self._output_log.clear()
        self._content_lines.clear()
        self._current_line = ""

    def set_thinking(self, thinking: bool) -> None:
        """
        Set the thinking state visibility.
        
        Args:
            thinking: True to show thinking indicator, False to hide.
        """
        self.is_thinking = thinking
        if self._thinking_indicator:
            self._thinking_indicator.visible = thinking

    def update_thinking_text(self, text: str) -> None:
        """
        Update the thinking indicator text.
        
        Args:
            text: The thinking state text to display.
        """
        self.current_thinking = text
        if self._thinking_indicator:
            self._thinking_indicator.update(text)

    def watch_is_thinking(self, thinking: bool) -> None:
        """
        React to thinking state changes.
        
        Args:
            thinking: The new thinking state.
        """
        if self._thinking_indicator:
            self._thinking_indicator.visible = thinking

    def watch_current_thinking(self, text: str) -> None:
        """
        React to thinking text changes.
        
        Args:
            text: The new thinking text.
        """
        if self._thinking_indicator:
            self._thinking_indicator.update(text)

    def stream_character(self, char: str) -> None:
        """
        Stream a single character.
        
        Args:
            char: The character to stream.
        """
        if char == "\n":
            # End of line, add to content
            if self._current_line:
                self._content_lines.append(self._current_line)
                self._current_line = ""
        else:
            self._current_line += char
        
        # Write character directly
        if self._output_log:
            self._output_log.write(char)

    def stream_text(self, text: str, scroll: bool = True) -> None:
        """
        Stream text progressively.
        
        Args:
            text: The text to stream.
            scroll: Whether to auto-scroll.
        """
        self.append_text(text, scroll)

    def add_anime_phrase(self, phrase: str, description: str = "") -> None:
        """
        Add an anime execution phrase to the output.
        
        Args:
            phrase: The anime phrase (e.g., "[ Bankai ]").
            description: Optional description text.
        """
        full_text = phrase
        if description:
            full_text += f" {description}"
        
        # Add styling for the phrase
        styled = f"[dim]{phrase}[/dim]"
        if description:
            styled += f" {description}"
        
        self.append_line(styled)

    def add_separator(self) -> None:
        """Add a visual separator line."""
        separator = "─" * 40
        self.append_line(f"[dim]{separator}[/dim]")

    def scroll_to_bottom(self) -> None:
        """Scroll to the bottom of the output."""
        if self._output_log:
            self._output_log.scroll_end(animate=False)

    def get_content(self) -> str:
        """
        Get all output content as a string.
        
        Returns:
            The complete output content.
        """
        return "\n".join(self._content_lines + [self._current_line])
