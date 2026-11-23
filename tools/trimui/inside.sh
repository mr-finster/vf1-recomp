#!/bin/bash
# Runs inside the Debian 11 arm64 container (see build.sh): the TrimUI Smart Pro / Brick Pro build. SDL3 (static,
# with the "mali" video driver of sdl3-mali/), FreeType (static), the game (-DVF_TRIMUI, the 3D on the CPU, libstdc++
# and libgcc linked in), opaco, and the packages. No ROMs or anything made from them: the player puts their own
# vf.zip (and model1io.zip) in VirtuaFighter/roms/.
set -e
SDL_VERSION=3.4.18
FT_VERSION=2.14.3
cd /src
D=build/trimui
T=tools/trimui
mkdir -p $D
J=$(nproc)

# SDL3 with the mali driver, built once (again when the driver changes); in the container's /tmp, installed to $D/sdl
if [ ! -f $D/sdl/lib/libSDL3.a ] || [ $T/sdl3-mali/SDL_malivideo.c -nt $D/sdl/lib/libSDL3.a ]; then
	rm -rf $D/sdl /tmp/sdl && mkdir -p /tmp/sdl
	tar xzf build/deps/SDL3.tar.gz -C /tmp/sdl
	sh $T/sdl3-mali/apply.sh /tmp/sdl/SDL3-$SDL_VERSION
	cmake -S /tmp/sdl/SDL3-$SDL_VERSION -B /tmp/sdl/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PWD/$D/sdl \
		-DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_C_FLAGS="-O2 -mcpu=cortex-a53" \
		-DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_TEST_LIBRARY=OFF \
		-DSDL_MALI_TRIMUI=ON -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_VULKAN=OFF -DSDL_GPU=OFF \
		-DSDL_OPENGL=OFF -DSDL_OPENGLES=ON -DSDL_RENDER_VULKAN=OFF -DSDL_RENDER_GPU=OFF \
		-DSDL_ALSA=ON -DSDL_ALSA_SHARED=ON -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF \
		-DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_LIBUDEV=OFF -DSDL_HIDAPI=OFF -DSDL_CAMERA=OFF > /tmp/sdl/cmake.log
	grep -q "#define SDL_VIDEO_DRIVER_MALI 1" /tmp/sdl/build/include-config-release/build_config/SDL_build_config.h ||
		{ echo "SDL3 was configured without the mali driver"; exit 1; }
	cmake --build /tmp/sdl/build -j$J >/dev/null
	cmake --install /tmp/sdl/build >/dev/null
fi
# FreeType (static), built once
if [ ! -f $D/ft/lib/libfreetype.a ]; then
	rm -rf /tmp/ft && mkdir -p /tmp/ft && tar xJf build/deps/freetype.tar.xz -C /tmp/ft
	cmake -S /tmp/ft/freetype-$FT_VERSION -B /tmp/ft/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PWD/$D/ft \
		-DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON \
		-DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON >/dev/null
	cmake --build /tmp/ft/build >/dev/null
	cmake --install /tmp/ft/build >/dev/null
fi

# the game: the firmware has glibc 2.33 (this container 2.31), its own libz, no X11/Wayland/KMS, no Vulkan
export PKG_CONFIG_PATH=$PWD/$D/sdl/lib/pkgconfig
CXXFLAGS="-std=c++20 -O3 -mcpu=cortex-a53 -DVF_TRIMUI -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function -Wno-psabi" \
make -j$J ARCH=aarch64 OBJ=$D/obj BIN=$D/bin GPU3D=none \
	SDL_CFLAGS="$(pkg-config --cflags sdl3)" SDL_LIBS="$(pkg-config --static --libs sdl3) -static-libstdc++ -static-libgcc" \
	FT_CFLAGS="-I$PWD/$D/ft/include/freetype2" FT_LIBS="$PWD/$D/ft/lib/libfreetype.a" \
	$D/bin/vf-native
strip -o $D/vf $D/bin/vf-native
glibc=$(objdump -T $D/vf | grep -oE 'GLIBC_2\.[0-9]+' | sort -Vu | tail -1)
echo "vf: $(du -h $D/vf | cut -f1), glibc up to $glibc, needs: $(objdump -p $D/vf | awk '/NEEDED/ {print $2}' | tr '\n' ' ')"
[ "${glibc#GLIBC_2.}" -le 33 ] || { echo "the game needs $glibc: the firmware has 2.33"; exit 1; }
# opaco: the display layer opaque while the game runs (the launcher's)
gcc -O2 -Wall -Wextra -o $D/opaco $T/opaco.c && strip $D/opaco

# the packages: one folder, for the stock firmware's Apps/ (config.json) and the custom firmwares' ports (game.json)
[ -d build/deps/SDL3-$SDL_VERSION ] || tar xzf build/deps/SDL3.tar.gz -C build/deps
[ -d build/deps/freetype-$FT_VERSION ] || tar xJf build/deps/freetype.tar.xz -C build/deps
mkdir -p build/release && sh tools/release/licenses.sh
grep -q "Sam Lantinga" build/release/LICENSES.txt && grep -q "The FreeType Project LICENSE" build/release/LICENSES.txt ||
	{ echo "LICENSES.txt isn't whole (SDL3's or FreeType's notice missing)"; exit 1; }
P=$D/VirtuaFighter
rm -rf $P && mkdir -p $P/roms $P/assets
cp $D/vf $D/opaco $P/
cp -R assets/ui assets/fonts assets/vf_map.txt $P/assets/
[ -d assets/backgrounds ] && cp -R assets/backgrounds $P/assets/
[ -d assets/icons ] && cp -R assets/icons $P/assets/   # (made from the game, so not in the repository)
cp assets/icon/icon.png $P/icon.png
cp $T/launch.sh $T/README.txt $T/settings-trimui.ini $T/start_game.txt build/release/LICENSES.txt $P/
cp $T/config.json $T/game.json $P/
: > "$P/roms/put vf.zip here"
chmod +x $P/vf $P/opaco $P/launch.sh
(cd $D && rm -f VirtuaFighterRecomp-trimui.zip && zip -qr VirtuaFighterRecomp-trimui.zip VirtuaFighter)
ls -la $D/VirtuaFighterRecomp-trimui.zip
