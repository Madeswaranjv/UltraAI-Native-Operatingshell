"""
Anime Execution Words Module

Contains the 50 anime-themed execution phrases used during streaming state.
These phrases simulate intelligent processing steps in the AI thinking engine.
"""

from typing import List
import random

# The 50 anime execution phrases
ANIME_WORDS: List[str] = [
    "Bankai",
    "Domain Expansion",
    "Hollow Purple",
    "Gear Fifth",
    "Gear Second",
    "Gear Fourth",
    "Ashura Mode",
    "Rinnegan Activation",
    "Mangekyo Sharingan",
    "Eternal Mangekyo",
    "Six Paths Mode",
    "Kurama Link",
    "Rasengan Surge",
    "Chidori Strike",
    "Getsuga Tensho",
    "Mugetsu",
    "Black Flash",
    "Reverse Cursed Technique",
    "Malevolent Shrine",
    "Infinity Void",
    "Limitless",
    "Blue Technique",
    "Red Technique",
    "Cursed Energy Flow",
    "Soul Reaper Form",
    "Quincy Vollstandig",
    "Zangetsu Release",
    "Flame Breathing",
    "Water Breathing",
    "Thunder Breathing",
    "Sun Breathing",
    "Beast Breathing",
    "Dragon Slayer Mode",
    "Spirit Gun",
    "Demon Form",
    "Full Counter",
    "Overdrive",
    "Hyper Mode",
    "Final Getsuga",
    "Godspeed",
    "Lightning Cloak",
    "Shadow Clone Surge",
    "Truth Seeking Orbs",
    "Meteor Strike",
    "Void Collapse",
    "Energy Convergence",
    "Quantum Break",
    "Infinity Pulse",
    "System Override",
    "Ultra Sync",
]


class AnimeWordRotator:
    """
    Rotates through anime execution phrases without repetition loops.
    Provides smooth phrase rotation for streaming state visualization.
    """

    def __init__(self):
        self._available_words: List[str] = []
        self._used_words: List[str] = []
        self._reset_pool()

    def _reset_pool(self) -> None:
        """Reset the word pool with all available phrases."""
        self._available_words = ANIME_WORDS.copy()
        self._used_words = []
        random.shuffle(self._available_words)

    def get_next(self) -> str:
        """
        Get the next anime execution phrase.
        
        Returns:
            str: The next anime phrase from the rotation.
        """
        if not self._available_words:
            self._reset_pool()

        word = self._available_words.pop()
        self._used_words.append(word)
        return word

    def get_random(self) -> str:
        """
        Get a completely random phrase (may repeat).
        
        Returns:
            str: A random anime phrase.
        """
        return random.choice(ANIME_WORDS)

    def get_formatted_prefix(self) -> str:
        """
        Get a formatted execution prefix with brackets.
        
        Returns:
            str: Formatted like "[ Bankai ]"
        """
        return f"[ {self.get_next()} ]"


# Global rotator instance for shared state
_rotator = AnimeWordRotator()


def get_anime_word() -> str:
    """Get the next anime execution phrase."""
    return _rotator.get_next()


def get_formatted_prefix() -> str:
    """Get a formatted execution prefix."""
    return _rotator.get_formatted_prefix()


def get_all_words() -> List[str]:
    """Get all 50 anime execution phrases."""
    return ANIME_WORDS.copy()
