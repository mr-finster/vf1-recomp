# Sourced by the packaging scripts (tools/<os>/package.sh for mac, win and linux, and
# tools/linux/container.sh) from the repository's root. A game has downloads when it has
# tools/games/<game>/release/game.sh, its settings:
#   TITLE        the app's name ("Virtua Fighter Recomp")
#   FILE         the .exe's, the Linux folder's and launcher's ($R/$FILE.sh) and the downloads' name
#   PROG         the make target built (build/bin/$PROG: <game>-native, or <game> on the interpreter)
#   MAC_ID       the Mac app's bundle identifier
#   COPYRIGHT    the .exe's copyright line
#   ICNS ICO PNG the Mac, Windows and Linux icons (optional)
#   README_MAC README_WIN README_LINUX each download's README.txt
#   ROMS_README  the Linux package's roms/README.txt (optional)
#   FAST_BUILD   1: LINUX_FAST=1 also makes a -O3 Linux package for the Deck's CPU to compare speed (optional)
# Every download has assets/ui, assets/fonts and the game's own assets/games/<game> (with whatever
# is there, including files kept out of the repository such as VF's backgrounds/).

# GAMES: the games named (all with downloads when none); exits on one that has none
release_games()
{
	ALL=$(for g in tools/games/*/release/game.sh; do [ -f "$g" ] && basename "$(dirname "$(dirname "$g")")"; done | tr '\n' ' ')
	GAMES=${*:-$ALL}
	for G in $GAMES; do
		case " $ALL" in *" $G "*) ;; *) echo "usage: $0 [game ...] (games: $ALL)" >&2; exit 2 ;; esac
	done
}

# the settings of game $1 (R: its release folder)
release_game()
{
	TITLE= FILE= PROG= MAC_ID= COPYRIGHT= ICNS= ICO= PNG=
	README_MAC= README_WIN= README_LINUX= ROMS_README= FAST_BUILD=
	R=tools/games/$1/release
	. ./$R/game.sh
}

# copies game $1's assets into the folder $2 (a package's assets/)
release_assets()
{
	mkdir -p "$2/games"
	cp -R assets/ui assets/fonts "$2/"
	[ ! -d assets/games/$1 ] || cp -R assets/games/$1 "$2/games/"
}
