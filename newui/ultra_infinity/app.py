"""
Ultra Infinity CLI - Main Application

A cognitive interface system designed as a thinking terminal.
Minimal, focused, and intelligent AI interface inspired by Claude Code.

Usage:
    python app.py

Features:
    - Single vertical flow layout (no sidebars, no panels)
    - Monochrome theme for focus
    - Thinking animation with state cycling
    - Streaming output with anime execution phrases
    - Async non-blocking UI updates
"""

import asyncio
import sys
from pathlib import Path

# Add project root to path for imports
project_root = Path(__file__).parent
sys.path.insert(0, str(project_root))

from textual.app import App
from textual.containers import Vertical
from textual.reactive import reactive

from components.header import Header
from components.output import OutputPanel
from components.input_bar import InputBar
from core.state_manager import get_state_manager, AppState, reset_state_manager
from core.streamer import StreamerEngine, StreamConfig


class UltraInfinityApp(App):
    """
    Main application for Ultra Infinity CLI.
    
    Layout:
        TOP:    Minimal header ("ULTRA ∞")
        MIDDLE: Scrollable output area (AI responses)
        BOTTOM: Fixed input bar (user input)
    
    States:
        IDLE:     Waiting for user input
        THINKING: AI is processing (animation)
        STREAMING: AI is generating response
        COMPLETE: Response generation finished
    """

    # CSS file for styling
    CSS_PATH = "styles.tcss"
    
    # Reactive properties
    current_state = reactive(AppState.IDLE)

    def __init__(self):
        """Initialize the Ultra Infinity application."""
        super().__init__()
        
        # Initialize state manager
        self._state_manager = get_state_manager()
        
        # Initialize streamer engine
        stream_config = StreamConfig(
            thinking_update_interval=0.2,
            streaming_char_delay=0.015,
            streaming_line_delay=0.08,
            anime_phrase_frequency=3,
            max_thinking_duration=1.5
        )
        self._streamer = StreamerEngine(stream_config)
        
        # Component references
        self._header: Header | None = None
        self._output_panel: OutputPanel | None = None
        self._input_bar: InputBar | None = None
        
        # Setup streamer callbacks
        self._setup_streamer_callbacks()
        
        # Setup state manager callbacks
        self._setup_state_callbacks()

    def _setup_streamer_callbacks(self) -> None:
        """Configure streamer engine callbacks for UI updates."""
        self._streamer.set_callbacks(
            on_thinking_update=self._on_thinking_update,
            on_stream_chunk=self._on_stream_chunk,
            on_stream_line=self._on_stream_line,
            on_complete=self._on_stream_complete,
            on_anime_phrase=self._on_anime_phrase
        )

    def _setup_state_callbacks(self) -> None:
        """Configure state manager callbacks."""
        self._state_manager.on_any_state_change(self._on_state_change)

    def compose(self):
        """
        Compose the application layout.
        
        Layout structure:
            Vertical container
            ├── Header (static)
            ├── OutputPanel (scrollable, expands)
            └── InputBar (fixed at bottom)
        """
        with Vertical(id="main-container"):
            # Top: Minimal header
            self._header = Header()
            yield self._header
            
            # Middle: Scrollable output area
            self._output_panel = OutputPanel()
            yield self._output_panel
            
            # Bottom: Fixed input bar
            self._input_bar = InputBar()
            yield self._input_bar

    def on_mount(self) -> None:
        """Called when the application is mounted."""
        # Get component references
        self._header = self.query_one(Header)
        self._output_panel = self.query_one(OutputPanel)
        self._input_bar = self.query_one(InputBar)
        
        # Focus input on mount
        self._input_bar.focus_input()
        
        # Display welcome message
        self._display_welcome()

    def _display_welcome(self) -> None:
        """Display the welcome message."""
        welcome_lines = [
            "",
            "  Ultra Infinity CLI - Cognitive Interface System",
            "",
            "  Type your query and press Enter to engage the reasoning engine.",
            "",
            "  Commands:",
            "    help          - Show usage information",
            "    clear         - Clear the output",
            "    quit/exit     - Exit the application",
            "",
        ]
        
        for line in welcome_lines:
            self._output_panel.append_line(f"[dim]{line}[/dim]")
        
        self._output_panel.add_separator()

    def on_input_bar_input_submitted(self, event: InputBar.InputSubmitted) -> None:
        """
        Handle input submission from the input bar.
        
        Args:
            event: The input submission event.
        """
        user_input = event.value.strip()
        
        if not user_input:
            return
        
        # Handle special commands
        if self._handle_command(user_input):
            return
        
        # Process normal input
        self._process_user_input(user_input)

    def _handle_command(self, command: str) -> bool:
        """
        Handle special commands.
        
        Args:
            command: The user input to check for commands.
            
        Returns:
            True if a command was handled, False otherwise.
        """
        cmd_lower = command.lower()
        
        if cmd_lower in ("quit", "exit", "q"):
            self.exit()
            return True
        
        elif cmd_lower == "clear":
            self._output_panel.clear_output()
            return True
        
        elif cmd_lower == "help":
            # Let the streamer handle this
            return False
        
        return False

    def _process_user_input(self, user_input: str) -> None:
        """
        Process user input through the streaming engine.
        
        Args:
            user_input: The user's input text.
        """
        # Display user input in output
        self._output_panel.append_line("")
        self._output_panel.append_line(f"[bold]> {user_input}[/bold]")
        self._output_panel.append_line("")
        
        # Disable input during processing
        self._input_bar.set_enabled(False)
        self._input_bar.set_placeholder("Processing...")
        
        # Start the streaming process
        asyncio.create_task(self._streamer.process_input(user_input))

    def _on_thinking_update(self, text: str) -> None:
        """
        Handle thinking state updates.
        
        Args:
            text: The current thinking state text.
        """
        self.call_from_thread(self._update_thinking_ui, text)

    def _update_thinking_ui(self, text: str) -> None:
        """Update the UI with thinking text (thread-safe)."""
        if self._output_panel:
            self._output_panel.set_thinking(True)
            self._output_panel.update_thinking_text(text)

    def _on_stream_chunk(self, chunk: str) -> None:
        """
        Handle streaming character chunks.
        
        Args:
            chunk: The character chunk to stream.
        """
        # Character streaming is handled by the line callback for simplicity
        pass

    def _on_stream_line(self, line: str) -> None:
        """
        Handle completed streaming lines.
        
        Args:
            line: The completed line to display.
        """
        self.call_from_thread(self._append_line_ui, line)

    def _append_line_ui(self, line: str) -> None:
        """Append a line to the output (thread-safe)."""
        if self._output_panel:
            # Hide thinking indicator when streaming starts
            if self._state_manager.is_streaming:
                self._output_panel.set_thinking(False)
            
            self._output_panel.append_line(line)

    def _on_anime_phrase(self, phrase: str) -> None:
        """
        Handle anime phrase injection.
        
        Args:
            phrase: The anime execution phrase.
        """
        self.call_from_thread(self._append_anime_phrase_ui, phrase)

    def _append_anime_phrase_ui(self, phrase: str) -> None:
        """Append an anime phrase to the output (thread-safe)."""
        if self._output_panel:
            self._output_panel.add_anime_phrase(phrase)

    def _on_stream_complete(self) -> None:
        """Handle stream completion."""
        self.call_from_thread(self._on_complete_ui)

    def _on_complete_ui(self) -> None:
        """Handle completion UI updates (thread-safe)."""
        # Re-enable input
        if self._input_bar:
            self._input_bar.set_enabled(True)
            self._input_bar.set_placeholder("Type your message...")
            self._input_bar.focus_input()
        
        # Hide thinking indicator
        if self._output_panel:
            self._output_panel.set_thinking(False)
        
        # Add separator
        self._output_panel.add_separator()
        
        # Reset state after a brief delay
        asyncio.create_task(self._reset_state_delayed())

    async def _reset_state_delayed(self) -> None:
        """Reset state to IDLE after a brief delay."""
        await asyncio.sleep(0.5)
        if self._state_manager.is_complete:
            self._state_manager.transition_to(AppState.IDLE)

    def _on_state_change(self, from_state: AppState, to_state: AppState) -> None:
        """
        Handle state changes.
        
        Args:
            from_state: The previous state.
            to_state: The new state.
        """
        self.current_state = to_state

    def watch_current_state(self, state: AppState) -> None:
        """
        React to state changes.
        
        Args:
            state: The new state.
        """
        # Update UI based on state
        if self._input_bar:
            if state == AppState.IDLE:
                self._input_bar.set_enabled(True)
            else:
                self._input_bar.set_enabled(False)

    def on_key(self, event) -> None:
        """
        Handle keyboard events.
        
        Args:
            event: The key event.
        """
        # Ctrl+C to cancel/exit
        if event.key == "ctrl+c":
            if self._state_manager.is_processing:
                self._streamer.cancel()
                self._output_panel.append_line("\n[dim][ Cancelled ][/dim]")
                self._input_bar.set_enabled(True)
                self._input_bar.focus_input()
            else:
                self.exit()
        
        # Ctrl+L to clear screen
        elif event.key == "ctrl+l":
            self._output_panel.clear_output()

    def action_quit(self) -> None:
        """Quit the application."""
        self.exit()

    def on_unmount(self) -> None:
        """Called when the application is unmounting."""
        # Clean up
        self._streamer.cancel()
        reset_state_manager()


def main():
    """Main entry point for the Ultra Infinity CLI."""
    app = UltraInfinityApp()
    app.run()


if __name__ == "__main__":
    main()
