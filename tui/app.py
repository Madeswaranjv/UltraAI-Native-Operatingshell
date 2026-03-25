from textual.app import App, ComposeResult
from textual import work
from textual.binding import Binding

from components.header import UltraHeader
from components.output import OutputPanel
from components.input_bar import UltraInput
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
        self.output_panel = self.query_one("#main-output", OutputPanel)
        self.streamer = EngineStreamer(self.output_panel, self.state_manager)
        self.intent_parser = IntentParser()

        # Start in session setup — hide output, show setup panel
        self.query_one("#main-output").display = False
        self.query_one("#bottom-input").display = False
        self.query_one("#session-setup").display = True

    # ── Session Setup ────────────────────────────────────────────────────────

    def on_session_setup_completed(self, message) -> None:
        """Fired when user finishes governance/policy setup."""
        self.session.apply(message.config)
        self.state_manager.set_mode(message.config["mode"])

        # Switch to main terminal view
        self.query_one("#session-setup").display = False
        self.query_one("#main-output").display = True
        self.query_one("#bottom-input").display = True
        self.query_one(ModeBar).set_mode(message.config["mode"])

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

    async def on_input_submitted(self, message) -> None:
        input_widget = message.input
        prompt = input_widget.value.strip()
        if not prompt:
            return
        if self.state_manager.current_state != AppState.IDLE:
            return

        input_widget.value = ""
        self.state_manager.set_state(AppState.THINKING)
        self.output_panel.add_user_message(prompt)
        self._dispatch(prompt)

    @work(exclusive=True)
    async def _dispatch(self, prompt: str) -> None:
        """Route to correct pipeline based on active mode."""
        mode = self.state_manager.current_mode
        if mode == UltraMode.ARCHITECTURAL:
            await self.streamer.run_architectural_pipeline(prompt, self.session)
        else:
            parsed = await self.intent_parser.parse(prompt, self.session)
            await self.streamer.run_user_driven_pipeline(prompt, parsed, self.session)

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
        self.query_one(ModeBar).set_mode(new_mode)
        label = "ARCHITECTURAL" if new_mode == UltraMode.ARCHITECTURAL else "USER-DRIVEN"
        self.output_panel.add_system_message(f"Switched to {label} MODE")

    def action_cancel(self) -> None:
        if self.state_manager.current_state not in (AppState.IDLE, AppState.COMPLETE):
            self.output_panel.add_system_message("Task cancelled.")
            self.state_manager.set_state(AppState.IDLE)


if __name__ == "__main__":
    UltraInfinityApp().run()
