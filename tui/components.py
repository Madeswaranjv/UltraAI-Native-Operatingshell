import math
import random
from textual.widgets import Static, RichLog
from rich.text import Text
from rich.align import Align

class PhasePanel(Static):
    """Left sidebar displaying cognitive phases."""
    
    PHASES = ["PLAN", "MICRO-PLAN", "EXECUTE", "VERIFY", "REFLECT", "REPLAN"]
    
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.active_idx = 0

    def set_active(self, phase: str):
        if phase in self.PHASES:
            self.active_idx = self.PHASES.index(phase)

    def update_frame(self, frame: int):
        # Calculate sine wave pulsing (0.0 to 1.0)
        pulse = (math.sin(frame * 0.15) + 1) / 2
        
        # Base #8a2be2 (138, 43, 226) -> pulse intensity
        r = int(138 * pulse)
        g = int(43 * pulse)
        b = int(226 * pulse)
        pulse_color = f"#{r:02x}{g:02x}{b:02x}"

        panel_text = Text("\n[ COGNITIVE PHASES ]\n\n", justify="center", style="bold #d3d3d3")
        
        for i, p in enumerate(self.PHASES):
            if i == self.active_idx:
                panel_text.append(f" ► {p} ◄ \n\n", style=f"bold {pulse_color}")
            else:
                panel_text.append(f"   {p}   \n\n", style="dim #555555")

        self.update(Align.center(panel_text, vertical="middle"))

class HollowPurpleCore(Static):
    """Canvas for the cinematic Hollow Purple animation sequence."""

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.width = 66  # Fixed internal canvas width for predictable math
        
        # Base sphere templates
        self.small_sphere = [
            "  ▓▓▓  ",
            " ▓▓▓▓▓ ",
            "  ▓▓▓  "
        ]
        
        self.large_sphere = [
            "    ███████    ",
            "  ███████████  ",
            " █████████████ ",
            "  ███████████  ",
            "    ███████    "
        ]

    def update_frame(self, global_frame: int, action_frame: int, is_active: bool):
        if not is_active:
            # Idle animation: faint deep purple dot breathing in the dark
            pulse = (math.sin(global_frame * 0.1) + 1) / 2
            hex_val = int(40 * pulse)
            idle_text = Text("\n\n\n.\n", style=f"#{hex_val:02x}00{hex_val:02x}", justify="center")
            self.update(Align.center(idle_text, vertical="middle"))
            return

        canvas = Text()
        
        # PHASE 1: Blue & Red Spheres moving inwards [Frames 0-20]
        if 0 <= action_frame <= 20:
            progress = action_frame / 20.0
            
            # Start at edges (0 and 59) and move towards center (26 and 33)
            left_pos = int(0 + (26 - 0) * progress)
            right_pos = int(59 - (59 - 33) * progress)

            canvas.append("\n\n") # Vertical centering padding
            for line in self.small_sphere:
                row = Text()
                row.append(" " * left_pos)
                row.append(line, style="bold #0055ff") # Blue
                
                # Calculate space between spheres
                gap = right_pos - left_pos - 7 # 7 is width of small sphere
                row.append(" " * max(0, gap))
                
                row.append(line, style="bold #ff0033") # Red
                canvas.append(row)
                canvas.append("\n")

        # PHASE 2: Collision & Flash [Frames 21-25]
        elif 21 <= action_frame <= 25:
            scatter_chars = ["*", "+", "×", "✧", "·"]
            canvas.append("\n\n")
            for _ in range(3):
                row = Text(" " * 28)
                flash_core = "".join(random.choice(scatter_chars) for _ in range(10))
                row.append(flash_core, style="bold #ffffff") # Blinding flash
                canvas.append(row)
                canvas.append("\n")

        # PHASE 3: Hollow Purple Expansion & Destruction [Frames 26-55]
        elif 26 <= action_frame:
            # Ease out calculation for beams and opacity
            prog = min(1.0, (action_frame - 26) / 10.0)
            beam_length = int(24 * prog)
            
            # Fade out in the last 10 frames
            opacity = 1.0 if action_frame < 45 else max(0, 1.0 - ((action_frame - 45) / 10.0))
            
            purple_hex = f"#{int(138*opacity):02x}{int(43*opacity):02x}{int(226*opacity):02x}"
            dark_purple = f"#{int(75*opacity):02x}00{int(130*opacity):02x}"
            
            canvas.append("\n")
            for i, line in enumerate(self.large_sphere):
                row = Text()
                # Center rows emit destruction lines
                if i in [1, 2, 3]:
                    row.append(" " * (26 - beam_length))
                    row.append("≡" * beam_length, style=dark_purple)
                    row.append(line, style=f"bold {purple_hex}")
                    row.append("≡" * beam_length, style=dark_purple)
                else:
                    row.append(" " * 26)
                    row.append(line, style=f"bold {purple_hex}")
                
                canvas.append(row)
                canvas.append("\n")

        self.update(Align.center(canvas, vertical="middle"))

class OutputStream(RichLog):
    """Terminal output stream simulating AI logging."""
    
    def on_mount(self):
        self.write(Text("SYSTEM INITIALIZED.", style="bold #d3d3d3"))
        self.write(Text("Awaiting topological parameters...", style="dim #888888"))

    def stream_line(self, text: str):
        if text.startswith(">>"):
            self.write(Text(f"\n{text}", style="bold #ffffff"))
        elif "Hollow Purple" in text:
            self.write(Text(f"[! WARNING !] {text}", style="bold #8a2be2"))
        elif text == "Complete.":
            self.write(Text(f"[✔] {text}", style="bold #00ffaa"))
        else:
            self.write(Text(f"[~] {text}", style="#cccccc"))