from textual.containers import Vertical, VerticalScroll
from textual.widgets import Static


class ActiveResponseView(Vertical):
    """Live response widget with independent status and body regions."""

    def __init__(self, **kwargs):
        super().__init__(classes="ai-msg", **kwargs)
        self.status_label = Static("", classes="ai-status")
        self.detail_label = Static("", classes="ai-detail")
        self.body_label = Static("", classes="ai-body")
        self._body_text = ""

    def compose(self):
        yield self.status_label
        yield self.detail_label
        yield self.body_label

    def update_status(self, banner: str = "", detail: str = "") -> None:
        self.status_label.display = bool(banner)
        self.detail_label.display = bool(detail)
        self.status_label.update(banner)
        self.detail_label.update(detail)

    def clear_status(self) -> None:
        self.update_status("", "")

    def set_body(self, text: str) -> None:
        self._body_text = text
        self.body_label.update(text)

    def append_body(self, text: str) -> None:
        if not text:
            return
        self.set_body(self._body_text + text)


class OutputPanel(VerticalScroll):
    """Scrollable output area for user messages and live AI responses."""

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.active_view: ActiveResponseView | None = None

    def add_user_message(self, text: str) -> None:
        self.mount(Static(f"> {text}", classes="user-msg"))
        self.scroll_end(animate=False)

    def add_system_message(self, text: str) -> None:
        self.mount(Static(f"  {text}", classes="system-msg"))
        self.scroll_end(animate=False)

    def mount_new_response(self) -> None:
        self.active_view = ActiveResponseView()
        self.mount(self.active_view)
        self.scroll_end(animate=False)

    def update_active_status(self, banner: str = "", detail: str = "") -> None:
        if self.active_view:
            self.active_view.update_status(banner, detail)
            self.scroll_end(animate=False)

    def clear_active_status(self) -> None:
        if self.active_view:
            self.active_view.clear_status()
            self.scroll_end(animate=False)

    def update_active(self, text: str) -> None:
        if self.active_view:
            self.active_view.set_body(text)
            self.scroll_end(animate=False)

    def append_active(self, text: str) -> None:
        if self.active_view:
            self.active_view.append_body(text)
            self.scroll_end(animate=False)

    def finalize_active(self, clear_status: bool = True) -> None:
        if self.active_view and clear_status:
            self.active_view.clear_status()
        self.active_view = None
        self.scroll_end(animate=False)
