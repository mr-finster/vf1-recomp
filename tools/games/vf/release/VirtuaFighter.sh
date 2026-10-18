#!/bin/sh
# Launcher (x86-64 Linux; a Non-Steam Game on the Deck): runs the game from its own folder (saves go in data/).
cd "$(dirname "$0")" && exec ./vf "$@"
