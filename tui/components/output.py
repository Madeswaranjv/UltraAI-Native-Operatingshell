from textual.containers import VerticalScroll
from textual.widgets import Static


class OutputPanel(VerticalScroll):
    """Scrollable output area — all message types preserved."""

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.active_label = None

    def add_user_message(self, text: str) -> None:
        self.mount(Static(f"> {text}", classes="user-msg"))
        self.scroll_end(animate=False)

    def add_system_message(self, text: str) -> None:
        self.mount(Static(f"  {text}", classes="system-msg"))
        self.scroll_end(animate=False)

    def mount_new_response(self) -> None:
        self.active_label = Static("", classes="ai-msg")
        self.mount(self.active_label)
        self.scroll_end(animate=False)

    def update_active(self, text: str) -> None:
        if self.active_label:
            self.active_label.update(text)
            self.scroll_end(animate=False)

    def finalize_active(self) -> None:
        self.active_label = None
        self.scroll_end(animate=False)
