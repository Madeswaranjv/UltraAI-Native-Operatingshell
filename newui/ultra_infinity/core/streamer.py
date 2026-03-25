"""
Streamer Engine Module

The core streaming engine that manages AI response generation.
Handles thinking animations, streaming output, and anime phrase injection.

Uses async functions for non-blocking UI updates and smooth streaming.
"""

import asyncio
import random
from typing import Optional, Callable, List
from dataclasses import dataclass

from core.state_manager import get_state_manager, AppState
from utils.thinking_states import ThinkingStateCycler
from utils.anime_words import AnimeWordRotator


@dataclass
class StreamConfig:
    """Configuration for the streaming engine."""
    thinking_update_interval: float = 0.2  # 200ms for thinking animation
    streaming_char_delay: float = 0.01  # Delay between characters
    streaming_line_delay: float = 0.05  # Delay between lines
    anime_phrase_frequency: int = 3  # Insert phrase every N lines
    max_thinking_duration: float = 2.0  # Max thinking phase duration


class StreamerEngine:
    """
    The main streaming engine for AI response generation.
    
    Manages:
    - Thinking animation cycles
    - Streaming text output
    - Anime execution phrase injection
    - State transitions
    """

    def __init__(self, config: Optional[StreamConfig] = None):
        """
        Initialize the streamer engine.
        
        Args:
            config: Optional stream configuration. Uses defaults if not provided.
        """
        self.config = config or StreamConfig()
        self._state_manager = get_state_manager()
        self._thinking_cycler = ThinkingStateCycler(use_infinity=True)
        self._anime_rotator = AnimeWordRotator()
        
        self._thinking_task: Optional[asyncio.Task] = None
        self._streaming_task: Optional[asyncio.Task] = None
        self._is_cancelled: bool = False
        
        # Callbacks for UI updates
        self._on_thinking_update: Optional[Callable[[str], None]] = None
        self._on_stream_chunk: Optional[Callable[[str], None]] = None
        self._on_stream_line: Optional[Callable[[str], None]] = None
        self._on_complete: Optional[Callable[[], None]] = None
        self._on_anime_phrase: Optional[Callable[[str], None]] = None

    def set_callbacks(
        self,
        on_thinking_update: Optional[Callable[[str], None]] = None,
        on_stream_chunk: Optional[Callable[[str], None]] = None,
        on_stream_line: Optional[Callable[[str], None]] = None,
        on_complete: Optional[Callable[[], None]] = None,
        on_anime_phrase: Optional[Callable[[str], None]] = None,
    ) -> None:
        """
        Set callback functions for UI updates.
        
        Args:
            on_thinking_update: Called with thinking text updates
            on_stream_chunk: Called with each character chunk
            on_stream_line: Called with each completed line
            on_complete: Called when streaming completes
            on_anime_phrase: Called when anime phrase is injected
        """
        self._on_thinking_update = on_thinking_update
        self._on_stream_chunk = on_stream_chunk
        self._on_stream_line = on_stream_line
        self._on_complete = on_complete
        self._on_anime_phrase = on_anime_phrase

    async def process_input(self, user_input: str) -> None:
        """
        Process user input through the full pipeline.
        
        Args:
            user_input: The user's input text.
        """
        self._is_cancelled = False
        
        try:
            # Phase 1: Thinking
            await self._run_thinking_phase()
            
            if self._is_cancelled:
                return
            
            # Phase 2: Streaming
            await self._run_streaming_phase(user_input)
            
            if self._is_cancelled:
                return
            
            # Phase 3: Complete
            self._state_manager.transition_to(AppState.COMPLETE)
            
            if self._on_complete:
                self._on_complete()
                
        except asyncio.CancelledError:
            pass
        except Exception as e:
            # Handle errors gracefully
            if self._on_stream_line:
                self._on_stream_line(f"\n[ Error: {str(e)} ]")
            self._state_manager.transition_to(AppState.IDLE)

    async def _run_thinking_phase(self) -> None:
        """Run the thinking animation phase."""
        self._state_manager.transition_to(AppState.THINKING)
        
        start_time = asyncio.get_event_loop().time()
        
        while not self._is_cancelled:
            # Check if thinking duration exceeded
            elapsed = asyncio.get_event_loop().time() - start_time
            if elapsed >= self.config.max_thinking_duration:
                break
            
            # Update thinking text
            thinking_text = self._thinking_cycler.get_current_text()
            if self._on_thinking_update:
                self._on_thinking_update(thinking_text)
            
            # Advance to next state
            self._thinking_cycler.next()
            
            # Wait for next update
            await asyncio.sleep(self.config.thinking_update_interval)

    async def _run_streaming_phase(self, user_input: str) -> None:
        """Run the streaming response phase."""
        self._state_manager.transition_to(AppState.STREAMING)
        
        # Generate simulated response based on input
        response_lines = self._generate_response(user_input)
        
        line_count = 0
        for line in response_lines:
            if self._is_cancelled:
                break
            
            # Inject anime phrase periodically
            if line_count > 0 and line_count % self.config.anime_phrase_frequency == 0:
                phrase = self._anime_rotator.get_formatted_prefix()
                if self._on_anime_phrase:
                    self._on_anime_phrase(phrase)
            
            # Stream the line character by character
            await self._stream_line(line)
            line_count += 1
            
            # Small delay between lines
            await asyncio.sleep(self.config.streaming_line_delay)

    async def _stream_line(self, line: str) -> None:
        """
        Stream a single line character by character.
        
        Args:
            line: The line to stream.
        """
        current_text = ""
        
        for char in line:
            if self._is_cancelled:
                break
            
            current_text += char
            
            if self._on_stream_chunk:
                self._on_stream_chunk(char)
            
            await asyncio.sleep(self.config.streaming_char_delay)
        
        # Add newline at end of line
        if self._on_stream_chunk:
            self._on_stream_chunk("\n")
        
        if self._on_stream_line:
            self._on_stream_line(line)

    def _generate_response(self, user_input: str) -> List[str]:
        """
        Generate a simulated AI response based on user input.
        
        Args:
            user_input: The user's input.
            
        Returns:
            List of response lines.
        """
        # Simple response generation based on input
        user_input_lower = user_input.lower()
        
        if "hello" in user_input_lower or "hi" in user_input_lower:
            return [
                "Greetings.",
                "",
                "I am Ultra Infinity, a cognitive interface system designed for",
                "intelligent reasoning and execution.",
                "",
                "How may I assist you today?"
            ]
        
        elif "black hole" in user_input_lower or "blackhole" in user_input_lower:
            return [
                "Black holes are regions of spacetime where gravity is so intense",
                "that nothing, not even light, can escape from within the event horizon.",
                "",
                "Key characteristics:",
                "",
                "  - Event Horizon: The boundary beyond which escape is impossible",
                "  - Singularity: A point of infinite density at the center",
                "  - Time Dilation: Time slows near the event horizon",
                "  - Hawking Radiation: Quantum particles that escape over time",
                "",
                "The study of black holes bridges general relativity and quantum mechanics,",
                "representing one of the deepest mysteries in modern physics."
            ]
        
        elif "help" in user_input_lower:
            return [
                "Ultra Infinity CLI - Available Commands:",
                "",
                "  Type any question or topic to engage the reasoning engine.",
                "",
                "Examples:",
                "  - 'Explain quantum computing'",
                "  - 'What is consciousness?'",
                "  - 'Analyze the implications of AGI'",
                "",
                "The system will process your input through multiple phases:",
                "  [thinking] → [streaming] → [complete]"
            ]
        
        elif "quantum" in user_input_lower:
            return [
                "Quantum mechanics describes nature at the smallest scales.",
                "",
                "Core principles:",
                "",
                "  1. Superposition: Particles exist in multiple states simultaneously",
                "  2. Entanglement: Linked particles affect each other instantly",
                "  3. Uncertainty: Certain properties cannot be known precisely",
                "",
                "Applications include quantum computing, cryptography, and precision sensing.",
                "The field challenges our classical intuition about reality."
            ]
        
        elif "consciousness" in user_input_lower:
            return [
                "Consciousness remains one of science's greatest mysteries.",
                "",
                "Philosophical perspectives:",
                "",
                "  - Materialism: Consciousness emerges from physical processes",
                "  - Dualism: Mind and matter are fundamentally separate",
                "  - Panpsychism: Consciousness is a fundamental property of matter",
                "",
                "Scientific approaches study neural correlates, integrated information,",
                "and computational models of awareness.",
                "",
                "The hard problem: Why does subjective experience exist at all?"
            ]
        
        else:
            # Generic response for unknown inputs
            return [
                f"Processing your inquiry about: {user_input}",
                "",
                "Analyzing the conceptual framework and relevant knowledge domains...",
                "",
                "Based on available information:",
                "",
                f"The topic '{user_input}' encompasses multiple dimensions of analysis.",
                "Consider refining your query with more specific parameters",
                "for a more targeted response.",
                "",
                "Type 'help' for usage guidance."
            ]

    def cancel(self) -> None:
        """Cancel the current streaming operation."""
        self._is_cancelled = True
        
        if self._thinking_task and not self._thinking_task.done():
            self._thinking_task.cancel()
        
        if self._streaming_task and not self._streaming_task.done():
            self._streaming_task.cancel()
        
        self._state_manager.transition_to(AppState.IDLE)

    def reset(self) -> None:
        """Reset the streamer engine to initial state."""
        self.cancel()
        self._thinking_cycler.reset()
        self._is_cancelled = False


# Global streamer instance
_streamer: Optional[StreamerEngine] = None


def get_streamer_engine(config: Optional[StreamConfig] = None) -> StreamerEngine:
    """Get the global streamer engine instance."""
    global _streamer
    if _streamer is None:
        _streamer = StreamerEngine(config)
    return _streamer


def reset_streamer_engine() -> None:
    """Reset the global streamer engine."""
    global _streamer
    _streamer = None
