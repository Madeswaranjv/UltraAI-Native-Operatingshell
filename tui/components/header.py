from textual.widgets import Static

class UltraHeader(Static):
    """Static, minimal header displaying the system title."""
    
    def compose(self):
        yield Static("ULTRA ∞", id="header-text")