"""
Header Component Module

Minimal static header component displaying the application title.
Styled with clean monochrome aesthetics.
"""

from textual.widgets import Static
from textual.reactive import reactive


class Header(Static):
    """
    Static header component for Ultra Infinity CLI.
    
    Displays the application title "ULTRA ∞" in a clean,
    minimal style consistent with the monochrome theme.
    
    Attributes:
        title: The header title text (reactive)
    """

    # Reactive property for the title
    title = reactive("ULTRA ∞")

    DEFAULT_CSS = """
    Header {
        width: 100%;
        height: 3;
        background: #0a0a0a;
        color: #ffffff;
        content-align: center middle;
        text-style: bold;
        border-bottom: solid #1f1f1f;
    }
    """

    def __init__(self, title: str = "ULTRA ∞"):
        """
        Initialize the header component.
        
        Args:
            title: The title to display. Defaults to "ULTRA ∞".
        """
        super().__init__()
        self.title = title

    def compose(self):
        """Compose the header layout."""
        yield Static(self.title, id="header-title")

    def watch_title(self, new_title: str) -> None:
        """
        React to title changes.
        
        Args:
            new_title: The new title value.
        """
        header_title = self.query_one("#header-title", Static)
        header_title.update(new_title)

    def set_title(self, new_title: str) -> None:
        """
        Update the header title.
        
        Args:
            new_title: The new title to display.
        """
        self.title = new_title

    def on_mount(self) -> None:
        """Called when the component is mounted."""
        # Ensure title is displayed on mount
        header_title = self.query_one("#header-title", Static)
        header_title.update(self.title)
