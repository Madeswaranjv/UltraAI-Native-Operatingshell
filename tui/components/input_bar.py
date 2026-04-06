from textual.binding import Binding
from textual.containers import Container
from textual.message import Message
from textual.widgets import TextArea


class UltraPromptSubmitted(Message):
    """Bubble a multiline prompt submission to the app."""

    def __init__(self, prompt: str, text_area: "UltraPromptArea") -> None:
        super().__init__()
        self.prompt = prompt
        self.text_area = text_area


class UltraPromptArea(TextArea):
    """Multiline prompt editor: Enter = submit, Shift+Enter = new line."""

    BINDINGS = [
        Binding("shift+enter", "newline", "New Line", show=False),
    ]

    def on_key(self, event) -> None:
        """Intercept Enter before TextArea's default newline handler fires."""
        if event.key == "enter":
            event.prevent_default()   # stops TextArea inserting \n
            self.action_submit_prompt()

    def action_submit_prompt(self) -> None:
        self.post_message(UltraPromptSubmitted(self.text, self))

    def action_newline(self) -> None:
        self.insert("\n")

    def clear_prompt(self) -> None:
        self.load_text("")


class UltraInput(Container):
    """Fixed-bottom input bar container."""

    def compose(self):
        yield UltraPromptArea(
            placeholder="> Enter = submit   Shift+Enter = new line",
            id="cmd-input",
            soft_wrap=True,
        )