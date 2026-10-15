#!/bin/bash
# The Windows release, cross-compiled on the Mac with MinGW-w64 (brew install mingw-w64):
# build/win/VirtuaFighterRecomp-windows.zip with VirtuaFighter.exe, SDL3.dll, the assets, README
# and LICENSES. No ROMs or anything made from them: the game recompiles the player's own set at
# start-up. SDL3 and FreeType come from the same checked sources as the Mac build (tools/mac/deps.sh);
# zlib is fetched once and checked against its published SHA-256.
set -e
cd "$(dirname "$0")/../.."
SDL_VERSION=3.4.18
FT_VERSION=2.14.3
ZLIB_VERSION=1.3.1
ZLIB_SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
T=x86_64-w64-mingw32
D=$PWD/build/win
mkdir -p $D
[ -f build/deps/SDL3.tar.gz ] || tools/mac/deps.sh

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
	tar xzf build/deps/SDL3.tar.gz -C $D && mv $D/SDL3-$SDL_VERSION $D/SDL-src
	cmake -S $D/SDL-src -B $D/sdl-build -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=$D/toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=$D/sdl -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_STATIC=OFF >/dev/null
	cmake --build $D/sdl-build -j8 >/dev/null
	cmake --install $D/sdl-build >/dev/null
fi
# FreeType (static), built once
if [ ! -f $D/ft/lib/libfreetype.a ]; then
	rm -rf $D/ft-src $D/ft-build
	tar xJf build/deps/freetype.tar.xz -C $D && mv $D/freetype-$FT_VERSION $D/ft-src
	cmake -S $D/ft-src -B $D/ft-build -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=$D/toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=$D/ft -DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON \
		-DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON >/dev/null
	cmake --build $D/ft-build -j8 >/dev/null
	cmake --install $D/ft-build >/dev/null
fi
# zlib (static), fetched and built once
if [ ! -f $D/zlib/lib/libz.a ]; then
	[ -f build/deps/zlib.tar.gz ] || curl -fsSL -o build/deps/zlib.tar.gz \
		https://github.com/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.gz
	echo "$ZLIB_SHA256  build/deps/zlib.tar.gz" | shasum -a 256 -c -
	rm -rf $D/zlib-src && tar xzf build/deps/zlib.tar.gz -C $D && mv $D/zlib-$ZLIB_VERSION $D/zlib-src
	make -C $D/zlib-src -f win32/Makefile.gcc PREFIX=$T- libz.a >/dev/null
	mkdir -p $D/zlib/lib $D/zlib/include
	cp $D/zlib-src/libz.a $D/zlib/lib/ && cp $D/zlib-src/zlib.h $D/zlib-src/zconf.h $D/zlib/include/
fi

# the icon, the manifest (UTF-8 file names) and the version (Properties > Details), as resources: an unsigned
# exe that says nothing about itself is what antivirus heuristics flag most
VER=${VF_VERSION:-$(git describe --tags --always 2>/dev/null || echo v0.0.0)}   # (VF_VERSION=v1.1.11 for a release)
V4=$(echo "$VER" | sed -E 's/^v//; s/-([0-9]+)-g[0-9a-f]+$/.\1/' | awk -F. '{ printf "%d,%d,%d,%d", $1, $2, $3, $4 }')
cat > $D/vf.rc <<EOF
1 ICON "$PWD/assets/icon/VF1.ico"
1 24 "$PWD/tools/win/VirtuaFighter.manifest"
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
      VALUE "CompanyName", "Virtua Fighter Recomp"
      VALUE "FileDescription", "Virtua Fighter Recomp"
      VALUE "FileVersion", "$VER"
      VALUE "InternalName", "VirtuaFighter"
      VALUE "LegalCopyright", "Unofficial fan-made recompilation. Not affiliated with or endorsed by SEGA."
      VALUE "OriginalFilename", "VirtuaFighter.exe"
      VALUE "ProductName", "Virtua Fighter Recomp"
      VALUE "ProductVersion", "$VER"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
EOF
$T-windres $D/vf.rc -O coff -o $D/vf_res.o

# the game: libgcc, libstdc++ and winpthreads linked in, so SDL3.dll is the only DLL it brings
make -j$(sysctl -n hw.ncpu) CC=$T-gcc CXX=$T-g++ HOSTCC=cc ARCH=x86_64 TARGET_OS=windows \
	OBJ=build/win/obj BIN=build/win/bin \
	EXTRA_CFLAGS="-I$D/zlib/include -D_USE_MATH_DEFINES" \
	LDLIBS="$D/vf_res.o -L$D/zlib/lib -lz -mwindows -static -Wl,--stack,16777216" \
	SDL_CFLAGS="-I$D/sdl/include" SDL_LIBS="$D/sdl/lib/libSDL3.dll.a" \
	FT_CFLAGS="-I$D/ft/include/freetype2" FT_LIBS="$D/ft/lib/libfreetype.a" \
	build/win/bin/vf-native

# the package
P=$D/VirtuaFighter
rm -rf $P && mkdir -p $P/assets
cp build/win/bin/vf-native.exe $P/VirtuaFighter.exe 2>/dev/null || cp build/win/bin/vf-native $P/VirtuaFighter.exe
$T-strip $P/VirtuaFighter.exe
cp $D/sdl/bin/SDL3.dll $P/
cp -R assets/ui assets/fonts assets/backgrounds assets/vf_map.txt $P/assets/
[ -d assets/icons ] && cp -R assets/icons $P/assets/   # (the secret characters' select icons: made from the game, so not in the repository)
cp tools/win/README.txt $P/
[ -f build/release/LICENSES.txt ] && cp build/release/LICENSES.txt $P/
rm -f $D/VirtuaFighterRecomp-windows.zip
(cd $D && zip -qr VirtuaFighterRecomp-windows.zip VirtuaFighter)
ls -la $P $D/VirtuaFighterRecomp-windows.zip
