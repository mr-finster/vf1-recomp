#!/bin/bash
# Runs inside the Steam Runtime SDK container (see build.sh): the release build for the Steam Deck
# (and x86-64 Linux). No ROMs or anything made from them: the game recompiles the player's own
# set at start-up, from the vf.zip they put in VirtuaFighter/roms/.
set -e
SDL_VERSION=3.4.18
FT_VERSION=2.14.3
D=build/deck
mkdir -p $D
export CC=gcc-14 CXX=g++-14

# SDL3 (shared, shipped next to the game), built once
if [ ! -f $D/sdl/lib/libSDL3.so.0 ]; then
	rm -rf $D/SDL-src $D/sdl-build
	tar xzf build/deps/SDL3.tar.gz -C $D && mv $D/SDL3-$SDL_VERSION $D/SDL-src
	cmake -S $D/SDL-src -B $D/sdl-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PWD/$D/sdl \
		-DCMAKE_INSTALL_LIBDIR=lib -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_STATIC=OFF >/dev/null
	cmake --build $D/sdl-build >/dev/null
	cmake --install $D/sdl-build >/dev/null
fi
# FreeType (static), built once
if [ ! -f $D/ft/lib/libfreetype.a ]; then
	rm -rf $D/ft-src $D/ft-build
	tar xJf build/deps/freetype.tar.xz -C $D && mv $D/freetype-$FT_VERSION $D/ft-src
	cmake -S $D/ft-src -B $D/ft-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PWD/$D/ft \
		-DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON \
		-DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON >/dev/null
	cmake --build $D/ft-build >/dev/null
	cmake --install $D/ft-build >/dev/null
fi

# the game (Musashi's opcode tables generator is rebuilt for Linux here)
make -j$(nproc) ARCH=x86_64 OBJ=$D/obj BIN=$D/bin \
	SDL_CFLAGS="-I$PWD/$D/sdl/include" \
	SDL_LIBS="-L$PWD/$D/sdl/lib -lSDL3 -lpthread -ldl "'-Wl,-rpath,\$$ORIGIN/lib' \
	FT_CFLAGS="-I$PWD/$D/ft/include/freetype2" FT_LIBS="$PWD/$D/ft/lib/libfreetype.a" \
	$D/bin/vf-native

# the package
P=$D/VirtuaFighter
rm -rf $P && mkdir -p $P/lib $P/roms $P/assets
cp $D/bin/vf-native $P/vf
strip $P/vf
cp -L $D/sdl/lib/libSDL3.so.0 $P/lib/
cp -R assets/ui assets/fonts assets/backgrounds assets/vf_map.txt $P/assets/
[ -d assets/icons ] && cp -R assets/icons $P/assets/   # (the secret characters' select icons: made from the game, so not in the repository)
cp assets/icon/icon.png $P/icon.png
cp tools/deck/README.txt tools/deck/VirtuaFighter.sh $P/
[ -f build/release/LICENSES.txt ] && cp build/release/LICENSES.txt $P/
chmod +x $P/vf $P/VirtuaFighter.sh
tar czf $D/VirtuaFighter-deck.tar.gz -C $D VirtuaFighter

# DECK_FAST=1: a second package for comparing speed, the same game built -O3 for the Deck's CPU
# (Zen 2: AVX2), with no fused multiply-adds so every result is the same as the normal build's
if [ -n "$DECK_FAST" ]; then
	CXXFLAGS="-std=c++20 -O3 -g -march=x86-64-v3 -mtune=znver2 -ffp-contract=off -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function" \
	make -j$(nproc) ARCH=x86_64 OBJ=$D/obj-fast BIN=$D/bin-fast \
		SDL_CFLAGS="-I$PWD/$D/sdl/include" \
		SDL_LIBS="-L$PWD/$D/sdl/lib -lSDL3 -lpthread -ldl "'-Wl,-rpath,\$$ORIGIN/lib' \
		FT_CFLAGS="-I$PWD/$D/ft/include/freetype2" FT_LIBS="$PWD/$D/ft/lib/libfreetype.a" \
		$D/bin-fast/vf-native
	F=$D/VirtuaFighter-fast
	rm -rf $F && cp -R $P $F
	cp $D/bin-fast/vf-native $F/vf && strip $F/vf
	tar czf $D/VirtuaFighter-deck-fast.tar.gz -C $D VirtuaFighter-fast
	ls -la $D/VirtuaFighter-deck-fast.tar.gz
fi
ls -la $P $D/VirtuaFighter-deck.tar.gz
