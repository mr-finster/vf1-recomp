#!/bin/sh
# Writes build/LICENSES.txt, every download's: the notices of everything built into the program
# (the sources tools/release/deps.sh fetches must be in build/deps: release_deps).
cd "$(dirname "$0")/../.."
. tools/release/deps.sh
OUT=${LICENSES_OUT:-build/LICENSES.txt}
sec() { printf '\n\n==============================================================================\n%s\n==============================================================================\n\n' "$1"; }
{
# the games with downloads (tools/games/<game>/release/game.sh): "A, B and C"
TITLES=$(for g in tools/games/*/release/game.sh; do (R=; . ./$g; echo "$TITLE"); done | awk '{ t[NR] = $0 } END { for (i = 1; i <= NR; i++) { sep = ", "; if (i == NR - 1) sep = " and "; if (i == NR) sep = ""; printf "%s%s", t[i], sep } }')
echo "Third-party software in $TITLES, and its licences."
sec "MAME: the NEC V60 core and disassembler, the Fujitsu MB86233 (TGP) core, the MultiPCM, and the Model 1 machine, video, coprocessor and sound board code it is based on (BSD-3-Clause)"
cat <<'BSD'
Copyright-holders: Farfetch'd, R. Belmont, Olivier Galibert, Miguel Angel Horna, and the MAME
team (https://github.com/mamedev/mame).

Redistribution and use in source and binary forms, with or without modification, are permitted
provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions
   and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice, this list of
   conditions and the following disclaimer in the documentation and/or other materials provided
   with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors may be used to
   endorse or promote products derived from this software without specific prior written
   permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
BSD
sec "ymfm (YM3438), Aaron Giles"; cat third_party/ymfm/LICENSE
sec "sljit, Zoltan Herczeg"; cat third_party/sljit/LICENSE
sec "Musashi (68000), Karl Stenerud"; sed -n '/LICENSE AND COPYRIGHT/,/THE SOFTWARE\./p' third_party/musashi/readme.txt
sec "Z80 core, Nicolas Allemand"; cat third_party/z80/LICENSE
sec "RmlUi"; cat third_party/rmlui/LICENSE.txt
sec "GLM"; cat third_party/glm/copying.txt
sec "stb_image, Sean Barrett (MIT option)"; sed -n '/ALTERNATIVE A - MIT License/,/^SOFTWARE\.$/p' third_party/stb/stb_image.h
sec "SDL 3 (zlib)"; cat $SDL_SRC/LICENSE.txt
sec "FreeType (FreeType License)"
echo "Portions of this software are copyright (c) The FreeType Project (www.freetype.org). All rights reserved."
echo; cat $FT_SRC/docs/FTL.TXT
sec "Barlow Condensed font (SIL Open Font License 1.1)"; cat assets/fonts/OFL.txt
} > "$OUT"
