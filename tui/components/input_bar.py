from textual.containers import Container
from textual.widgets import Input

class UltraInput(Container):
    """Fixed-bottom input bar container."""
    
    def compose(self):
        yield Input(placeholder="> User input here...", id="cmd-input")