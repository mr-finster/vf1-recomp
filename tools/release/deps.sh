# The third-party sources the downloads are built with, the same for every platform (sourced by
# tools/<os>/package.sh, tools/linux/container.sh and tools/release/licenses.sh, from the
# repository's root). release_deps fetches them once into build/deps, checks each against its
# published SHA-256 and unpacks it there (LICENSES.txt quotes their licences from those folders).
SDL_VERSION=3.4.18
SDL_SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
FT_VERSION=2.14.3
FT_SHA256=36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f
ZLIB_VERSION=1.3.1                # (Windows only: MinGW has none)
ZLIB_SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
DEPS=build/deps
SDL_SRC=$DEPS/SDL3-$SDL_VERSION FT_SRC=$DEPS/freetype-$FT_VERSION ZLIB_SRC=$DEPS/zlib-$ZLIB_VERSION

# url, file in build/deps, SHA-256 (exits on a download that fails or doesn't match)
release_fetch()
{
	[ -f "$DEPS/$2" ] || curl -fsSL -o "$DEPS/$2" "$1" || { echo "couldn't fetch $1" >&2; exit 1; }
	SUM=$( (shasum -a 256 "$DEPS/$2" 2>/dev/null || sha256sum "$DEPS/$2") | cut -d' ' -f1)
	[ "$SUM" = "$3" ] || { echo "$DEPS/$2: SHA-256 $SUM, not $3" >&2; exit 1; }
	echo "$DEPS/$2: OK"
}

# SDL3 and FreeType, and zlib too with "zlib", fetched and unpacked once
release_deps()
{
	mkdir -p $DEPS
	release_fetch https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz SDL3.tar.gz $SDL_SHA256
	release_fetch https://downloads.sourceforge.net/project/freetype/freetype2/$FT_VERSION/freetype-$FT_VERSION.tar.xz freetype.tar.xz $FT_SHA256
	[ -d $SDL_SRC ] || tar xzf $DEPS/SDL3.tar.gz -C $DEPS || exit 1
	[ -d $FT_SRC ] || tar xJf $DEPS/freetype.tar.xz -C $DEPS || exit 1
	if [ "$1" = zlib ]; then
		release_fetch https://github.com/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.gz zlib.tar.gz $ZLIB_SHA256
		[ -d $ZLIB_SRC ] || tar xzf $DEPS/zlib.tar.gz -C $DEPS || exit 1
	fi
}
