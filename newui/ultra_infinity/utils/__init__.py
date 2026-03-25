"""
Utils Package

Utility modules for Ultra Infinity CLI.
"""

from utils.anime_words import (
    ANIME_WORDS,
    AnimeWordRotator,
    get_anime_word,
    get_formatted_prefix,
    get_all_words,
)
from utils.thinking_states import (
    ThinkingState,
    ThinkingStateCycler,
    ThinkingStateType,
    get_thinking_text,
    next_thinking_text,
    reset_thinking_states,
)

__all__ = [
    # Anime words
    "ANIME_WORDS",
    "AnimeWordRotator",
    "get_anime_word",
    "get_formatted_prefix",
    "get_all_words",
    # Thinking states
    "ThinkingState",
    "ThinkingStateCycler",
    "ThinkingStateType",
    "get_thinking_text",
    "next_thinking_text",
    "reset_thinking_states",
]
