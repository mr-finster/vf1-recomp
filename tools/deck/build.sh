#!/bin/sh
# Builds the Steam Deck package (SteamOS: x86-64 Linux) in Valve's Steam Runtime 3 "sniper"
# SDK container, so the binary runs against the libraries every Steam install has.
#
#   tools/deck/build.sh        -> build/deck/VirtuaFighter/ and build/deck/VirtuaFighterRecomp-deck.tar.gz
#
# Needs Docker able to run linux/amd64 images. The package has nothing from the ROMs in it: the
# player puts their own vf.zip in VirtuaFighter/roms/ and the game recompiles it when it starts.
set -e
cd "$(dirname "$0")/../.."
IMG=registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest
[ -f build/deps/SDL3.tar.gz ] || tools/mac/deps.sh     # fetches the SDL3 and FreeType sources
# the menu's version and date, from git here (the container may have no git)
VF_VERSION=${VF_VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}   # (VF_VERSION=v1.1.11 for a release)
VF_DATE=$(git log -1 --format=%cd --date=format:'%b %-d %Y' 2>/dev/null)
VF_COMMIT=$(git rev-parse --short HEAD 2>/dev/null)
docker run --rm --platform linux/amd64 -e VF_VERSION="$VF_VERSION" -e VF_DATE="$VF_DATE" -e VF_COMMIT="$VF_COMMIT" -e DECK_FAST="$DECK_FAST" -v "$PWD":/src -w /src "$IMG" bash tools/deck/inside.sh
