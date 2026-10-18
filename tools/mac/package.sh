#!/bin/sh
# The Mac downloads, each a zip with the app (universal: Apple silicon and Intel, macOS 11 and later,
# ad-hoc signed: players right-click > Open the first time), README.txt and LICENSES.txt:
#   tools/mac/package.sh [game ...]   ->  build/mac/<game>/<FILE>Recomp-mac.zip
#                                         (VirtuaFighterRecomp-mac.zip, VirtuaRacingRecomp-mac.zip)
# With no game named, every game with downloads is packaged (each one's settings:
# tools/games/<game>/release/game.sh). Nothing from the ROMs goes in them: the maps contain
# addresses only, and each game asks for the player's own set at launch. The one exception, the
# author's call (2026-10-07): assets/games/vf/backgrounds, VF's widescreen sides painted for the title and select.
set -e
cd "$(dirname "$0")/../.."
. tools/release/games.sh
. tools/release/deps.sh
release_games "$@"
release_deps
tools/release/licenses.sh   # build/LICENSES.txt
VERSION=${M1_VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}   # (M1_VERSION=v1.1.11 for a release)
D=build/mac
mkdir -p $D

# SDL3 and FreeType (static, built into the program, both architectures), built once
L=$PWD/$D/libs
if [ ! -f $L/lib/libSDL3.a ]; then
	cmake -S $SDL_SRC -B $D/sdl-build -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$L" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
	cmake --build $D/sdl-build -j10 && cmake --install $D/sdl-build
fi
if [ ! -f $L/lib/libfreetype.a ]; then
	cmake -S $FT_SRC -B $D/ft-build -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
		-DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$L" -DBUILD_SHARED_LIBS=OFF \
		-DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON
	cmake --build $D/ft-build -j10 && cmake --install $D/ft-build
fi
FW="-Wl,-framework,CoreMedia -Wl,-framework,CoreVideo -Wl,-framework,Cocoa -Wl,-weak_framework,UniformTypeIdentifiers \
-Wl,-framework,IOKit -Wl,-framework,ForceFeedback -Wl,-framework,Carbon -Wl,-framework,CoreAudio -Wl,-framework,AudioToolbox \
-Wl,-framework,AVFoundation -Wl,-framework,Foundation -Wl,-framework,GameController -Wl,-framework,Metal \
-Wl,-framework,QuartzCore -Wl,-weak_framework,CoreHaptics"
# the opcode-table generator runs here, on this Mac: use ours rather than one built for each target
make -s build/bin/m68kmake

for G in $GAMES; do
	release_game $G
	NAME=$TITLE ID=$MAC_ID ICON=$ICNS ZIP=${FILE}Recomp-mac.zip
	W=$D/$G

	# the game, for each architecture, joined into one universal program
	for A in arm64 x86_64; do
		mkdir -p $W/$A/bin
		cp -p build/bin/m68kmake $W/$A/bin/m68kmake
		make -s -j10 ARCH=$A OBJ=$W/$A/obj BIN=$W/$A/bin \
			CC="clang -arch $A -mmacosx-version-min=11.0" CXX="clang++ -arch $A -mmacosx-version-min=11.0" \
			SDL_CFLAGS="-I$L/include" SDL_LIBS="$L/lib/libSDL3.a $FW -liconv" \
			FT_CFLAGS="-I$L/include/freetype2" FT_LIBS="$L/lib/libfreetype.a" \
			$W/$A/bin/$PROG
	done
	lipo -create $W/arm64/bin/$PROG $W/x86_64/bin/$PROG -output $W/$PROG
	lipo -archs $W/$PROG

	# the package: the folder the zip holds, the app and the two text files in it
	P="$W/$NAME"
	APP="$P/$NAME.app"
	rm -rf "$P" && mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/assets"
	cp $W/$PROG "$APP/Contents/MacOS/$NAME"
	release_assets $G "$APP/Contents/Resources/assets"
	ICON_KEY=
	if [ -n "$ICON" ]; then
		cp "$ICON" "$APP/Contents/Resources/"
		ICON_KEY="<key>CFBundleIconFile</key><string>$(basename "$ICON" .icns)</string>"
	fi
	cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>$NAME</string>
	<key>CFBundleDisplayName</key><string>$NAME</string>
	<key>CFBundleExecutable</key><string>$NAME</string>
	<key>CFBundleIdentifier</key><string>$ID</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	$ICON_KEY
	<key>CFBundleShortVersionString</key><string>$VERSION</string>
	<key>CFBundleVersion</key><string>$VERSION</string>
	<key>LSMinimumSystemVersion</key><string>11.0</string>
	<key>NSHighResolutionCapable</key><true/>
	<key>LSApplicationCategoryType</key><string>public.app-category.arcade-games</string>
	<key>GCSupportsControllerUserInteraction</key><true/>
</dict>
</plist>
PLIST
	codesign --force --deep -s - "$APP"
	cp $README_MAC "$P/README.txt"
	cp build/LICENSES.txt "$P/LICENSES.txt"
	rm -f "$W/$ZIP"
	(cd "$W" && ditto -c -k --norsrc --keepParent "$NAME" "$ZIP")
	ls -la "$P" "$W/$ZIP"
done
