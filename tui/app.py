import asyncio

from textual.app import App, ComposeResult
from textual import work
from textual.binding import Binding
from textual.worker import get_current_worker

from components.header import UltraHeader
from components.output import OutputPanel
from components.input_bar import UltraInput, UltraPromptSubmitted
from components.mode_bar import ModeBar
from components.session_setup import SessionSetup
from core.streamer import EngineStreamer
from core.state_manager import StateManager, AppState, UltraMode
from core.intent_parser import IntentParser
from core.session import UltraSession


class UltraInfinityApp(App):
    """The Ultra Infinity Cognitive Terminal."""

    CSS_PATH = "styles.tcss"

    BINDINGS = [
        Binding("ctrl+c", "quit", "Quit", show=False),
        Binding("ctrl+s", "toggle_mode", "Switch Mode", show=False),
        Binding("ctrl+v", "toggle_verbose", "Verbose", show=False),
        Binding("escape", "cancel", "Cancel", show=False),
    ]

    def compose(self) -> ComposeResult:
        yield UltraHeader()
        yield ModeBar()
        yield SessionSetup(id="session-setup")
        yield OutputPanel(id="main-output")
        yield UltraInput(id="bottom-input")

    def on_mount(self) -> None:
        self.state_manager = StateManager()
        self.session = UltraSession()
        self.mode_bar = self.query_one(ModeBar)
        self.output_panel = self.query_one("#main-output", OutputPanel)
        self.streamer = EngineStreamer(self.output_panel, self.state_manager)
        self.intent_parser = IntentParser()
        self._active_worker = None
        self.state_manager.add_listener(self._on_state_changed)
        self._on_state_changed(self.state_manager.current_state)
        self.output_panel.set_verbose(self.state_manager.verbose_stream)

        # Start in session setup — hide output, show setup panel
        self.query_one("#main-output").display = False
        self.query_one("#bottom-input").display = False
        self.query_one("#session-setup").display = True

    def _on_state_changed(self, state: AppState) -> None:
        self.mode_bar.set_state(self.state_manager.state_label)

    # ── Session Setup ────────────────────────────────────────────────────────

    def on_session_setup_completed(self, message) -> None:
        """Fired when user finishes governance/policy setup."""
        self.session.apply(message.config)
        self.state_manager.set_mode(message.config["mode"])

        # Switch to main terminal view
        self.query_one("#session-setup").display = False
        self.query_one("#main-output").display = True
        self.query_one("#bottom-input").display = True
        self.mode_bar.set_mode(message.config["mode"])

        self.streamer.set_session(self.session)
        self._show_welcome()
        self.query_one("#cmd-input").focus()

    def _show_welcome(self) -> None:
        mode = self.session.mode
        if mode == UltraMode.ARCHITECTURAL:
            self.output_panel.add_system_message(
                "ARCHITECTURAL MODE — Provide your project structure and Ultra will plan autonomously within your governance rules."
            )
        else:
            self.output_panel.add_system_message(
                "USER-DRIVEN MODE — Describe what you want. Ultra will parse your intent, build a task graph, and execute."
            )

    # ── Input Handling ───────────────────────────────────────────────────────

    async def on_ultra_prompt_submitted(self, message: UltraPromptSubmitted) -> None:
        input_widget = message.text_area
        prompt = message.prompt
        if not prompt.strip():
            return
        if self.state_manager.current_state != AppState.IDLE:
            return

        input_widget.clear_prompt()
        self.state_manager.set_state(AppState.THINKING)
        self.output_panel.add_user_message(prompt.rstrip("\n"))
        self._active_worker = self._dispatch(prompt)

    @work(exclusive=True)
    async def _dispatch(self, prompt: str) -> None:
        """Route to correct pipeline based on active mode."""
        worker = get_current_worker()
        try:
            mode = self.state_manager.current_mode
            if mode == UltraMode.ARCHITECTURAL:
                await self.streamer.run_architectural_pipeline(prompt, self.session)
            else:
                parsed = await self.intent_parser.parse(prompt, self.session)
                await self.streamer.run_user_driven_pipeline(prompt, parsed, self.session)
        except asyncio.CancelledError:
            self.output_panel.finalize_active()
            self.output_panel.add_system_message("Task cancelled.")
            raise
        except Exception as exc:  # noqa: BLE001
            self.output_panel.finalize_active()
            self.output_panel.add_system_message(f"Backend failure: {exc}")
            self.state_manager.set_state(AppState.ERROR)
        finally:
            if self._active_worker is worker:
                self._active_worker = None
            self.state_manager.set_state(AppState.IDLE)
            self.query_one("#cmd-input").focus()

    # ── Keybindings ──────────────────────────────────────────────────────────

    def action_toggle_mode(self) -> None:
        if self.state_manager.current_state != AppState.IDLE:
            return
        new_mode = (
            UltraMode.USER_DRIVEN
            if self.state_manager.current_mode == UltraMode.ARCHITECTURAL
            else UltraMode.ARCHITECTURAL
        )
        self.state_manager.set_mode(new_mode)
        self.mode_bar.set_mode(new_mode)
        label = "ARCHITECTURAL" if new_mode == UltraMode.ARCHITECTURAL else "USER-DRIVEN"
        self.output_panel.add_system_message(f"Switched to {label} MODE")

    def action_cancel(self) -> None:
        if self.state_manager.current_state not in (AppState.IDLE, AppState.COMPLETE):
            if self._active_worker is not None and not self._active_worker.is_finished:
                self.output_panel.add_system_message("Cancelling task...")
                self._active_worker.cancel()
            else:
                self.output_panel.add_system_message("Task cancelled.")
                self.state_manager.set_state(AppState.IDLE)

    def action_toggle_verbose(self) -> None:
        enabled = not self.state_manager.verbose_stream
        self.state_manager.set_verbose_stream(enabled)
        self.output_panel.set_verbose(enabled)
        label = "enabled" if enabled else "disabled"
        self.output_panel.add_system_message(f"Verbose cognitive logs {label}.")


if __name__ == "__main__":
    UltraInfinityApp().run()
