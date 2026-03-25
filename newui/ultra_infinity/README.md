# Ultra Infinity CLI

A cognitive interface system designed as a **thinking terminal**. Minimal, focused, and intelligent AI interface inspired by Claude Code.

## Philosophy

This is NOT a dashboard. This is a **thinking terminal**.

The UI is designed to feel:
- **Focused** - Single vertical flow, no distractions
- **Silent** - No visual noise, only essential elements
- **Intelligent** - AI thinking and streaming states
- **Minimal but powerful** - Everything you need, nothing you don't

## Layout

```
┌──────────────────────────────┐
│ ULTRA ∞                     │  <- Minimal header (static)
├──────────────────────────────┤
│                              │
│   AI RESPONSE STREAM         │  <- Scrollable output area
│                              │     (MOST important component)
│                              │
├──────────────────────────────┤
│ > User input here...         │  <- Fixed input bar (always visible)
└──────────────────────────────┘
```

## Features

### States

1. **IDLE** - Waiting for user input
2. **THINKING** - AI is processing (animated indicator)
3. **STREAMING** - AI is generating response
4. **COMPLETE** - Response generation finished

### Thinking Animation

Cycles through infinity-themed states:
- `[ ∞ thinking ]`
- `[ ∞ expanding ]`
- `[ ∞ resolving ]`
- `[ ∞ converging ]`

### Streaming with Anime Execution Phrases

During response generation, the system injects anime-themed execution phrases:

```
[ Domain Expansion ] Generating structure...
[ Hollow Purple ] Compressing reasoning...
[ Bankai ] Releasing output...
```

All 50 phrases available:
Bankai, Domain Expansion, Hollow Purple, Gear Fifth, Gear Second, Gear Fourth, Ashura Mode, Rinnegan Activation, Mangekyo Sharingan, Eternal Mangekyo, Six Paths Mode, Kurama Link, Rasengan Surge, Chidori Strike, Getsuga Tensho, Mugetsu, Black Flash, Reverse Cursed Technique, Malevolent Shrine, Infinity Void, Limitless, Blue Technique, Red Technique, Cursed Energy Flow, Soul Reaper Form, Quincy Vollstandig, Zangetsu Release, Flame Breathing, Water Breathing, Thunder Breathing, Sun Breathing, Beast Breathing, Dragon Slayer Mode, Spirit Gun, Demon Form, Full Counter, Overdrive, Hyper Mode, Final Getsuga, Godspeed, Lightning Cloak, Shadow Clone Surge, Truth Seeking Orbs, Meteor Strike, Void Collapse, Energy Convergence, Quantum Break, Infinity Pulse, System Override, Ultra Sync

## Installation

### Requirements

- Python 3.10+
- Textual framework

### Setup

```bash
# Clone or navigate to the project directory
cd ultra_infinity

# Install dependencies
pip install textual

# Run the application
python app.py
```

## Usage

### Basic Commands

- Type any question or topic to engage the reasoning engine
- Press `Enter` to submit
- `help` - Show usage information
- `clear` - Clear the output
- `quit`, `exit`, or `q` - Exit the application

### Keyboard Shortcuts

- `Ctrl+C` - Cancel current operation (or exit if idle)
- `Ctrl+L` - Clear screen

### Example Interactions

```
> Explain black holes

[ ∞ thinking ]
[ ∞ expanding ]
[ Domain Expansion ]
Black holes are regions of spacetime where gravity is so intense
that nothing, not even light, can escape from within the event horizon.
...
```

## Architecture

### File Structure

```
ultra_infinity/
│
├── app.py                 # Main application entry point
├── styles.tcss            # Monochrome theme styles
├── __init__.py            # Package initialization
│
├── components/            # UI components
│   ├── __init__.py
│   ├── header.py          # Static header component
│   ├── output.py          # Scrollable output panel
│   └── input_bar.py       # Fixed input bar
│
├── core/                  # Core functionality
│   ├── __init__.py
│   ├── state_manager.py   # State management (IDLE, THINKING, STREAMING, COMPLETE)
│   └── streamer.py        # Async streaming engine
│
├── utils/                 # Utility modules
│   ├── __init__.py
│   ├── anime_words.py     # 50 anime execution phrases
│   └── thinking_states.py # Thinking state cycles
│
└── README.md              # This file
```

### Design Principles

1. **Single Vertical Flow** - No sidebars, no panels, no split layouts
2. **Monochrome Theme** - Pure black background, white/gray text
3. **Minimal Animations** - Only during thinking and streaming phases
4. **Async Non-Blocking** - Smooth UI updates during processing
5. **Clean State Transitions** - Well-defined state machine

## Color System

| Element | Color |
|---------|-------|
| Background | `#000000` (pure black) |
| Header background | `#0a0a0a` (near black) |
| Borders | `#1f1f1f` (subtle gray) |
| Primary text | `#ffffff` (white) |
| Secondary text | `#e6e6e6` (light gray) |
| Dim text | `#666666` (gray) |

## Technical Details

### Framework

- **Textual** - Modern Python framework for building Terminal User Interfaces

### Key Features

- Async/await for non-blocking UI
- Reactive state management
- Smooth text streaming
- Scrollable output with auto-follow
- Fixed input bar always visible

### State Machine

```
    ┌─────────┐
    │  IDLE   │◄─────────────────┐
    └────┬────┘                  │
         │                       │
         │ User input            │
         ▼                       │
    ┌─────────┐                  │
    │THINKING │──────────────────┤
    └────┬────┘  (cancel)        │
         │                       │
         │ Thinking complete     │
         ▼                       │
    ┌─────────┐                  │
    │STREAMING│──────────────────┤
    └────┬────┘  (cancel)        │
         │                       │
         │ Streaming complete    │
         ▼                       │
    ┌─────────┐                  │
    │COMPLETE │──────────────────┘
    └─────────┘  (auto-reset)
```

## License

MIT License - Feel free to use and modify as needed.

## Credits

Inspired by:
- Claude Code (Anthropic)
- High-end developer tools
- Minimal terminal interfaces

---

**Ultra Infinity CLI** - Think. Stream. Execute.
