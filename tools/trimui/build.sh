#!/bin/sh
# Builds the TrimUI Smart Pro / Brick Pro package (Allwinner A133P, ARM64, PowerVR GE8300) in a Debian 11 arm64
# container (Dockerfile beside this), so the game runs on the consoles' firmware (glibc 2.33):
#
#   tools/trimui/build.sh   -> build/trimui/VirtuaFighter/ and build/trimui/VirtuaFighterRecomp-trimui.zip
#
# Needs Docker able to run linux/arm64 images (native on Apple silicon and arm64 Linux, emulated elsewhere). The
# package has nothing from the ROMs in it: the player puts their own vf.zip (and model1io.zip) in VirtuaFighter/roms/.
set -e
cd "$(dirname "$0")/../.."
IMG=vf1-trimui-build
[ -f build/deps/SDL3.tar.gz ] || tools/mac/deps.sh     # fetches the SDL3 and FreeType sources
docker image inspect $IMG >/dev/null 2>&1 || docker build --platform linux/arm64 -t $IMG tools/trimui
VF_VERSION=${VF_VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}
VF_DATE=$(git log -1 --format=%cd --date=format:'%b %-d %Y' 2>/dev/null)
VF_COMMIT=$(git rev-parse --short HEAD 2>/dev/null)
docker run --rm --platform linux/arm64 -e VF_VERSION="$VF_VERSION" -e VF_DATE="$VF_DATE" -e VF_COMMIT="$VF_COMMIT" \
	-v "$PWD":/src -w /src $IMG bash tools/trimui/inside.sh
