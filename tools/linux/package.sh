#!/bin/sh
# Builds the x86-64 Linux package (the Steam Deck's SteamOS included) in Valve's Steam Runtime 3 "sniper"
# SDK container, so the binary runs against the libraries every Steam install has.
#
#   tools/linux/package.sh [game ...]   ->  build/linux/<game>/<FILE>Recomp-linux.tar.gz
#                                           (VirtuaFighterRecomp-linux.tar.gz, VirtuaRacingRecomp-linux.tar.gz)
#
# With no game named, every game with downloads is built (each one's settings:
# tools/games/<game>/release/game.sh).
#
# Needs Docker able to run linux/amd64 images. Packages contain no ROMs: each reads the player's own
# MAME set (vf.zip, vr.zip) when it starts.
set -e
cd "$(dirname "$0")/../.."
. tools/release/games.sh
. tools/release/deps.sh
release_games "$@"
release_deps                  # (fetched here: the container may have no network tools)
tools/release/licenses.sh     # build/LICENSES.txt
IMG=registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest
# the menu's version and date, from git here (the container may have no git)
M1_VERSION=${M1_VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}   # (M1_VERSION=v1.1.11 for a release)
M1_DATE=$(git log -1 --format=%cd --date=format:'%b %-d %Y' 2>/dev/null)
docker run --rm --platform linux/amd64 -e M1_VERSION="$M1_VERSION" -e M1_DATE="$M1_DATE" -e LINUX_FAST="$LINUX_FAST" -e GAMES="$GAMES" -v "$PWD":/src -w /src "$IMG" bash tools/linux/container.sh
