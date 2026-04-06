import time
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
        if text == self._body_text:
            return
        self._body_text = text
        self.body_label.update(text)

    def append_body(self, text: str) -> None:
        if not text:
            return
        self._body_text += text
        self.body_label.update(self._body_text)

class OutputPanel(VerticalScroll):
    """Scrollable output area for user messages and live AI responses."""

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.active_view: ActiveResponseView | None = None
        self._last_scroll_time = 0.0
        self._scroll_interval_seconds = 0.05

    def _maybe_scroll_end(self, force: bool = False) -> None:
        now = time.monotonic()
        if force or now - self._last_scroll_time >= self._scroll_interval_seconds:
            self.scroll_end(animate=False)
            self._last_scroll_time = now

    def _prefix_block(self, prefix: str, text: str) -> str:
        lines = text.splitlines() or [text]
        return "\n".join(f"{prefix}{line}" for line in lines)

    def add_user_message(self, text: str) -> None:
        self.mount(Static(self._prefix_block("> ", text), classes="user-msg"))
        self._maybe_scroll_end(force=True)

    def add_system_message(self, text: str) -> None:
        self.mount(Static(self._prefix_block("  ", text), classes="system-msg"))
        self._maybe_scroll_end(force=True)

    def mount_new_response(self) -> None:
        self.active_view = ActiveResponseView()
        self.mount(self.active_view)
        self._maybe_scroll_end(force=True)

    def update_active_status(self, banner: str = "", detail: str = "") -> None:
        if self.active_view:
            self.active_view.update_status(banner, detail)
            self._maybe_scroll_end()

    def clear_active_status(self) -> None:
        if self.active_view:
            self.active_view.clear_status()
            self._maybe_scroll_end()

    def update_active(self, text: str) -> None:
        if self.active_view:
            self.active_view.set_body(text)
            self._maybe_scroll_end()

    def append_active(self, text: str) -> None:
        if self.active_view:
            self.active_view.append_body(text)
            self._maybe_scroll_end()

    def finalize_active(self, clear_status: bool = True) -> None:
        if self.active_view and clear_status:
            self.active_view.clear_status()
        self.active_view = None
        self._maybe_scroll_end(force=True)
