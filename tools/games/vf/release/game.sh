# Virtua Fighter's downloads (read by tools/release/games.sh for tools/mac/package.sh,
# tools/win/package.sh and tools/linux/package.sh). Paths are from the repository's root.
TITLE="Virtua Fighter Recomp"     # the app's name
FILE=VirtuaFighter                # the .exe, the Linux folder and its launcher ($R/VirtuaFighter.sh), the downloads
PROG=vf-native                    # what's built (make's build/bin/vf-native): native code made at start-up
MAC_ID=recomp.vf1.unofficial
COPYRIGHT="Unofficial fan-made recompilation. Not affiliated with or endorsed by SEGA."
ICNS=assets/games/vf/icon/VF1.icns
ICO=assets/games/vf/icon/VF1.ico
PNG=assets/games/vf/icon/icon.png
# Copied with the rest of assets/games/vf: backgrounds/, not in the repository, when there (the
# widescreen sides painted for the title and select, the author's call, 2026-10-07), and select_icons/.
README_MAC=$R/README-mac.txt
README_WIN=$R/README-windows.txt
README_LINUX=$R/README-linux.txt
FAST_BUILD=1                      # LINUX_FAST=1 also makes VirtuaFighterRecomp-linux-fast.tar.gz
