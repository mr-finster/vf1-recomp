#!/bin/sh
# Builds the Mac download: build/release/VirtuaFighterRecomp-mac.zip with the app (universal,
# macOS 11+, ad-hoc signed: players right-click > Open the first time), README.txt and LICENSES.txt.
# Nothing from the ROMs goes in it: assets/vf_map.txt is addresses only. The one exception, the author's
# call (2026-10-07): assets/backgrounds, the widescreen sides painted for the title and select.
set -e
cd "$(dirname "$0")/../.."
tools/mac/build_release.sh
APP="build/release/Virtua Fighter Recomp.app"
rm -rf build/release && mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/assets"
cp build/rel/vf-native "$APP/Contents/MacOS/Virtua Fighter Recomp"
cp -R assets/ui assets/fonts assets/backgrounds assets/vf_map.txt "$APP/Contents/Resources/assets/"
[ -d assets/icons ] && cp -R assets/icons "$APP/Contents/Resources/assets/"   # (the secret characters' select icons: made from the game, so not in the repository)
cp assets/icon/VF1.icns "$APP/Contents/Resources/VF1.icns"
VERSION=${VF_VERSION:-$(git describe --tags --always 2>/dev/null || echo dev)}   # (VF_VERSION=v1.1.11 for a release)
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>Virtua Fighter Recomp</string>
	<key>CFBundleDisplayName</key><string>Virtua Fighter Recomp</string>
	<key>CFBundleExecutable</key><string>Virtua Fighter Recomp</string>
	<key>CFBundleIdentifier</key><string>recomp.vf1.unofficial</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleIconFile</key><string>VF1</string>
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
cp tools/release/README.txt build/release/README.txt
tools/release/licenses.sh
# one zip holding the app folder and the two text files
(cd build/release && rm -f VirtuaFighterRecomp-mac.zip && mkdir -p pkg/"Virtua Fighter Recomp" && \
 cp -R "Virtua Fighter Recomp.app" README.txt LICENSES.txt pkg/"Virtua Fighter Recomp"/ && \
 ditto -c -k --norsrc --keepParent "pkg/Virtua Fighter Recomp" VirtuaFighterRecomp-mac.zip && rm -rf pkg)
ls -la build/release
