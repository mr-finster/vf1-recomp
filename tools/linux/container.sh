#!/bin/bash
# Runs inside the Steam Runtime SDK container (see package.sh): the x86-64 Linux release build (SteamOS
# on the Steam Deck included), for the games in $GAMES (every game with downloads by default: their settings
# are tools/games/<game>/release/game.sh). No ROMs or anything made from them: each game reads the
# player's own set at start-up, from the zip they put in its roms/.
set -e
. tools/release/games.sh
. tools/release/deps.sh   # (the sources: package.sh fetched them)
release_games $GAMES
D=build/linux
mkdir -p "$D"
export CC=gcc-14 CXX=g++-14

# SDL3 (shared, shipped next to the game), built once
if [ ! -f $D/sdl/lib/libSDL3.so.0 ]; then
	rm -rf $D/SDL-src $D/sdl-build
	cp -R $SDL_SRC $D/SDL-src
	cmake -S $D/SDL-src -B $D/sdl-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PWD/$D/sdl \
		-DCMAKE_INSTALL_LIBDIR=lib -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_STATIC=OFF >/dev/null
	cmake --build $D/sdl-build >/dev/null
	cmake --install $D/sdl-build >/dev/null
fi
# FreeType (static), built once
if [ ! -f $D/ft/lib/libfreetype.a ]; then
	rm -rf $D/ft-src $D/ft-build
	cp -R $FT_SRC $D/ft-src
	cmake -S $D/ft-src -B $D/ft-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PWD/$D/ft \
		-DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON \
		-DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON >/dev/null
	cmake --build $D/ft-build >/dev/null
	cmake --install $D/ft-build >/dev/null
fi

# the games (Musashi's opcode tables generator is rebuilt for Linux here)
MAKE_DEPS=(SDL_CFLAGS="-I$PWD/$D/sdl/include"
	SDL_LIBS="-L$PWD/$D/sdl/lib -lSDL3 -lpthread -ldl "'-Wl,-rpath,\$$ORIGIN/lib'
	FT_CFLAGS="-I$PWD/$D/ft/include/freetype2" FT_LIBS="$PWD/$D/ft/lib/libfreetype.a")

for G in $GAMES; do
	release_game $G
	DIR=$FILE BINARY=$G TAR=${FILE}Recomp-linux.tar.gz
	W=$D/$G
	mkdir -p "$W"
	make -j$(nproc) ARCH=x86_64 OBJ=$W/obj BIN=$W/bin "${MAKE_DEPS[@]}" $W/bin/$PROG

	# the package: the player's zip goes in its roms folder (Game Mode has no file picker)
	P=$W/$DIR
	rm -rf "$P" && mkdir -p "$P/lib" "$P/roms" "$P/assets"
	cp $W/bin/$PROG "$P/$BINARY"
	strip "$P/$BINARY"
	cp -L $D/sdl/lib/libSDL3.so.0 "$P/lib/"
	release_assets $G "$P/assets"
	[ -z "$PNG" ] || cp $PNG "$P/icon.png"
	cp $R/$DIR.sh "$P/"
	cp $README_LINUX "$P/README.txt"
	[ -z "$ROMS_README" ] || cp $ROMS_README "$P/roms/README.txt"
	cp build/LICENSES.txt "$P/"
	chmod +x "$P/$BINARY" "$P/$DIR.sh"
	tar czf $W/$TAR -C $W $DIR

	# LINUX_FAST=1: a second package for comparing speed on the Deck (games with FAST_BUILD=1), the same game built
	# -O3 for the Deck's CPU (Zen 2: AVX2), with no fused multiply-adds so every result is the same as
	# the normal build's
	if [ -n "$FAST_BUILD" ] && [ -n "$LINUX_FAST" ]; then
		CXXFLAGS="-std=c++20 -O3 -g -march=x86-64-v3 -mtune=znver2 -ffp-contract=off -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function" \
		make -j$(nproc) ARCH=x86_64 OBJ=$W/obj-fast BIN=$W/bin-fast "${MAKE_DEPS[@]}" $W/bin-fast/$PROG
		F=$W/$DIR-fast
		rm -rf "$F" && cp -R "$P" "$F"
		cp $W/bin-fast/$PROG "$F/$BINARY" && strip "$F/$BINARY"
		tar czf $W/${FILE}Recomp-linux-fast.tar.gz -C $W $DIR-fast
		ls -la $W/${FILE}Recomp-linux-fast.tar.gz
	fi
	ls -la "$P" $W/$TAR
done
