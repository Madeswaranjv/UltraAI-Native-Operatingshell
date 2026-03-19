import asyncio
from textual.app import App, ComposeResult
from textual.containers import Horizontal, Vertical
from textual.widgets import Footer, Static, Input
from textual.reactive import reactive

# Import custom components
from components import PhasePanel, HollowPurpleCore, OutputStream

class UltraInfinity(App):
    """ULTRA INFINITY — Cognitive Interface"""

    CSS_PATH = "styles.tcss"

    # Reactive states for animation and UI
    global_frame = reactive(0)
    action_frame = reactive(0)
    is_animating = reactive(False)
    current_status = reactive("IDLE")

    def compose(self) -> ComposeResult:
        """Compose the cinematic layout."""
        # HEADER
        with Horizontal(id="header"):
            yield Static("ULTRA ∞", id="logo")
            yield Static(f"MODE: ACTIVE | STATUS: {self.current_status}", id="status")

        # MAIN AREA
        with Horizontal(id="main-area"):
            yield PhasePanel(id="left-panel")

            with Vertical(id="right-panel"):
                yield HollowPurpleCore(id="hollow-purple-core")
                yield OutputStream(id="output-stream")
                yield Input(placeholder=">> Enter command to initiate process...", id="command-input")

        # FOOTER
        yield Footer()

    def on_mount(self) -> None:
        """Start the animation engine (12.5 FPS)."""
        self.set_interval(0.08, self.tick_frame)

    def tick_frame(self) -> None:
        """Global animation loop driving all mathematical rendering."""
        self.global_frame += 1
        
        if self.is_animating:
            self.action_frame += 1
            # Reset after animation completes (Phase 3 ends around frame 50-55)
            if self.action_frame > 55:
                self.is_animating = False
                self.action_frame = 0

        # Safely push frame data to custom widgets
        try:
            self.query_one(PhasePanel).update_frame(self.global_frame)
            self.query_one(HollowPurpleCore).update_frame(
                self.global_frame, self.action_frame, self.is_animating
            )
        except Exception:
            pass # Ignore errors during initial widget mounting

    def watch_current_status(self, old_value: str, new_value: str) -> None:
        """Automatically update the header status text when the reactive variable changes."""
        try:
            self.query_one("#status", Static).update(f"MODE: ACTIVE | STATUS: {new_value}")
        except Exception:
            pass

    async def on_input_submitted(self, event: Input.Submitted) -> None:
        """Trigger the cinematic AI evaluation sequence."""
        command = event.value.strip()
        if not command:
            return

        input_widget = self.query_one(Input)
        input_widget.value = ""
        input_widget.disabled = True # Prevent interruption

        output = self.query_one(OutputStream)
        phases = self.query_one(PhasePanel)

        # Initiate animation states
        self.current_status = "EVALUATING"
        self.is_animating = True
        self.action_frame = 0

        # 5-step sequence syncing with the 50-frame animation (~4 seconds)
        sequence = [
            f">> {command}",
            "Analyzing topology...",
            "Decomposing parameters...",
            "Fusing Reversal and Amplification...",
            "Executing Hollow Purple...",
            "Complete."
        ]

        phase_mapping = ["PLAN", "MICRO-PLAN", "EXECUTE", "VERIFY", "REFLECT"]

        # 50 frames @ 0.08s = 4.0 seconds total. 5 steps = 0.8s per step.
        for i, step in enumerate(sequence[1:]):
            active_phase = phase_mapping[min(i, len(phase_mapping) - 1)]
            phases.set_active(active_phase)
            
            output.stream_line(step)
            await asyncio.sleep(0.8)

        phases.set_active("REPLAN")
        self.current_status = "IDLE"
        input_widget.disabled = False
        input_widget.focus()

if __name__ == "__main__":
    app = UltraInfinity()
    app.run()