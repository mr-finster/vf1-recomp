#!/bin/bash
# The Windows releases, cross-compiled on the Mac with MinGW-w64 (brew install mingw-w64):
#   tools/win/package.sh [game ...]   ->  build/win/<game>/<FILE>Recomp-windows.zip
#                                         (VirtuaFighterRecomp-windows.zip, VirtuaRacingRecomp-windows.zip)
# With no game named, every game with downloads is built (each one's settings:
# tools/games/<game>/release/game.sh). Each holds the game's .exe, SDL3.dll, the assets, README and
# LICENSES. No ROMs or anything made from them: each game reads the player's own set at start-up.
# SDL3, FreeType and zlib: the checked sources every platform builds from (tools/release/deps.sh).
set -e
cd "$(dirname "$0")/../.."
. tools/release/games.sh
. tools/release/deps.sh
release_games "$@"
release_deps zlib
tools/release/licenses.sh   # build/LICENSES.txt
T=x86_64-w64-mingw32
D=$PWD/build/win
mkdir -p "$D"

cat > $D/toolchain.cmake <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER $T-gcc)
set(CMAKE_CXX_COMPILER $T-g++)
set(CMAKE_RC_COMPILER $T-windres)
set(CMAKE_FIND_ROOT_PATH $(dirname $(dirname $(which $T-gcc)))/$T)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

# SDL3 (SDL3.dll, shipped next to the game), built once
if [ ! -f $D/sdl/bin/SDL3.dll ]; then
	rm -rf $D/SDL-src $D/sdl-build
	cp -R $SDL_SRC $D/SDL-src
	cmake -S $D/SDL-src -B $D/sdl-build -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=$D/toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=$D/sdl -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_STATIC=OFF >/dev/null
	cmake --build $D/sdl-build -j8 >/dev/null
	cmake --install $D/sdl-build >/dev/null
fi
# FreeType (static), built once
if [ ! -f $D/ft/lib/libfreetype.a ]; then
	rm -rf $D/ft-src $D/ft-build
	cp -R $FT_SRC $D/ft-src
	cmake -S $D/ft-src -B $D/ft-build -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=$D/toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=$D/ft -DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON \
		-DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON >/dev/null
	cmake --build $D/ft-build -j8 >/dev/null
	cmake --install $D/ft-build >/dev/null
fi
# zlib (static), built once
if [ ! -f $D/zlib/lib/libz.a ]; then
	rm -rf $D/zlib-src && cp -R $ZLIB_SRC $D/zlib-src
	make -C $D/zlib-src -f win32/Makefile.gcc PREFIX=$T- libz.a >/dev/null
	mkdir -p $D/zlib/lib $D/zlib/include
	cp $D/zlib-src/libz.a $D/zlib/lib/ && cp $D/zlib-src/zlib.h $D/zlib-src/zconf.h $D/zlib/include/
fi

# the icon, the manifest (UTF-8 file names) and the version (Properties > Details), as resources: an unsigned
# exe that says nothing about itself is what antivirus heuristics flag most
VER=${M1_VERSION:-$(git describe --tags --always 2>/dev/null || echo v0.0.0)}   # (M1_VERSION=v1.1.11 for a release)
V4=$(echo "$VER" | sed -E 's/^v//; s/-([0-9]+)-g[0-9a-f]+$/.\1/' | awk -F. '{ printf "%d,%d,%d,%d", $1, $2, $3, $4 }')

for G in $GAMES; do
	release_game $G
	EXE=$FILE ZIP=${FILE}Recomp-windows.zip
	ICON=; [ -z "$ICO" ] || ICON="1 ICON \"$PWD/$ICO\""
	W=$D/$G
	mkdir -p "$W"
	# the manifest (tools/win/app.manifest), named for the game
	sed "s/@NAME@/${FILE}Recomp/" tools/win/app.manifest > "$W/$G.manifest"
	cat > "$W/$G.rc" <<RC
$ICON
1 24 "$W/$G.manifest"
1 VERSIONINFO
FILEVERSION $V4
PRODUCTVERSION $V4
FILEOS 0x40004
FILETYPE 0x1
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904b0"
    BEGIN
      VALUE "CompanyName", "$TITLE"
      VALUE "FileDescription", "$TITLE"
      VALUE "FileVersion", "$VER"
      VALUE "InternalName", "$EXE"
      VALUE "LegalCopyright", "$COPYRIGHT"
      VALUE "OriginalFilename", "$EXE.exe"
      VALUE "ProductName", "$TITLE"
      VALUE "ProductVersion", "$VER"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
RC
	$T-windres "$W/$G.rc" -O coff -o "$W/${G}_res.o"

	# the game: libgcc, libstdc++ and winpthreads linked in, so SDL3.dll is the only DLL it brings
	make -j$(sysctl -n hw.ncpu) CC=$T-gcc CXX=$T-g++ HOSTCC=cc ARCH=x86_64 TARGET_OS=windows \
		OBJ=$W/obj BIN=$W/bin \
		EXTRA_CFLAGS="-I$D/zlib/include -D_USE_MATH_DEFINES" \
		LDLIBS="$W/${G}_res.o -L$D/zlib/lib -lz -mwindows -static -Wl,--stack,16777216" \
		SDL_CFLAGS="-I$D/sdl/include" SDL_LIBS="$D/sdl/lib/libSDL3.dll.a" \
		FT_CFLAGS="-I$D/ft/include/freetype2" FT_LIBS="$D/ft/lib/libfreetype.a" \
		$W/bin/$PROG

	# the package
	P=$W/$EXE
	rm -rf "$P" && mkdir -p "$P/assets"
	cp "$W/bin/$PROG.exe" "$P/$EXE.exe" 2>/dev/null || cp "$W/bin/$PROG" "$P/$EXE.exe"
	$T-strip "$P/$EXE.exe"
	cp $D/sdl/bin/SDL3.dll "$P/"
	release_assets $G "$P/assets"
	cp $README_WIN "$P/README.txt"
	cp build/LICENSES.txt "$P/"
	rm -f "$W/$ZIP"
	(cd "$W" && zip -qr "$ZIP" "$EXE")
	ls -la "$P" "$W/$ZIP"
done
