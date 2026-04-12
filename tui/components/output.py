import time
from textual.containers import Vertical, VerticalScroll
from textual.widgets import Static


class ActiveResponseView(Vertical):
    """Live response widget with independent status and body regions."""

    def __init__(self, **kwargs):
        super().__init__(classes="ai-msg", **kwargs)
        self.status_label = Static("", classes="ai-status")
        self.detail_label = Static("", classes="ai-detail")
        self.stream_label = Static("", classes="ai-stream")
        self.body_label = Static("", classes="ai-body")
        self.raw_label = Static("", classes="ai-raw")
        self._stream_text = ""
        self._body_text = ""
        self._raw_text = ""
        self._verbose = False

    def compose(self):
        yield self.status_label
        yield self.detail_label
        yield self.stream_label
        yield self.body_label
        yield self.raw_label

    def update_status(self, banner: str = "", detail: str = "") -> None:
        self.status_label.display = bool(banner)
        self.detail_label.display = bool(detail)
        self.status_label.update(banner)
        self.detail_label.update(detail)
        self._refresh_visibility()

    def clear_status(self) -> None:
        self.update_status("", "")

    def set_stream(self, text: str) -> None:
        if text == self._stream_text:
            return
        self._stream_text = text
        self.stream_label.update(text)
        self._refresh_visibility()

    def append_stream(self, text: str) -> None:
        if not text:
            return
        self._stream_text += text
        self.stream_label.update(self._stream_text)
        self._refresh_visibility()

    def set_body(self, text: str) -> None:
        if text == self._body_text:
            return
        self._body_text = text
        self.body_label.update(text)
        self._refresh_visibility()

    def append_body(self, text: str) -> None:
        if not text:
            return
        self._body_text += text
        self.body_label.update(self._body_text)
        self._refresh_visibility()

    def set_raw(self, text: str) -> None:
        if text == self._raw_text:
            return
        self._raw_text = text
        self.raw_label.update(text)
        self._refresh_visibility()

    def append_raw(self, text: str) -> None:
        if not text:
            return
        self._raw_text += text
        self.raw_label.update(self._raw_text)
        self._refresh_visibility()

    def set_verbose(self, enabled: bool) -> None:
        self._verbose = enabled
        self._refresh_visibility()

    def _refresh_visibility(self) -> None:
        self.stream_label.display = bool(self._stream_text)
        self.body_label.display = bool(self._body_text)
        self.raw_label.display = self._verbose and bool(self._raw_text)

class OutputPanel(VerticalScroll):
    """Scrollable output area for user messages and live AI responses."""

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.active_view: ActiveResponseView | None = None
        self._last_scroll_time = 0.0
        self._scroll_interval_seconds = 0.05
        self._verbose_mode = False

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

    def mount_new_response(self, verbose: bool = False) -> None:
        self._verbose_mode = verbose
        self.active_view = ActiveResponseView()
        self.active_view.set_verbose(verbose)
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

    def update_active_stream(self, text: str) -> None:
        if self.active_view:
            self.active_view.set_stream(text)
            self._maybe_scroll_end()

    def append_active_stream(self, text: str) -> None:
        if self.active_view:
            self.active_view.append_stream(text)
            self._maybe_scroll_end()

    def update_active_raw(self, text: str) -> None:
        if self.active_view:
            self.active_view.set_raw(text)
            self._maybe_scroll_end()

    def append_active_raw(self, text: str) -> None:
        if self.active_view:
            self.active_view.append_raw(text)
            self._maybe_scroll_end()

    def set_verbose(self, enabled: bool) -> None:
        self._verbose_mode = enabled
        if self.active_view:
            self.active_view.set_verbose(enabled)
            self._maybe_scroll_end()

    def finalize_active(self, clear_status: bool = True) -> None:
        if self.active_view and clear_status:
            self.active_view.clear_status()
        self.active_view = None
        self._maybe_scroll_end(force=True)
