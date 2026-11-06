# Netplay: VF1 online, as VF3's

VF1 plays online the way VF3 Recomp and VF2 Recomp do: the same rollback library (`src/net`, kept the same in all
three games), the same lobby (vf3-recomp's `tools/lobby`, a Cloudflare Worker: `game` "vf1"), the same Online
screens and the same words. What's VF1's own is below. VF3's `docs/netplay.md` has the shared parts in full:
rollback, Auto input delay, keeping in time, the handshake, the lobby, privacy and safety.

## What's VF1's own

- **The save state** (`model1::save_state` / `load_state`, `src/machine/model1_state.cpp`, `state_io.h`): the V60,
  the TGP (its program's RAMs, FIFOs and registers, or MAME 0.210's simulation), the RAMs, the display lists, the
  palette, the tile chip, the timers and IRQs, the UART and the I/O stand-in; 1.1 MB, saved in 0.1 ms, restored in
  0.12. Not in it: the picture's own state (the 3D's uploads, smooth motion, the skies: drawn from the board, never
  read by the game), the sound board, the real I/O board, the cheats' host-side state (all off in a match).
- **The recompiled code's stack** (`rec::save_stack` / `load_stack`): the recompiled V60 runs on a coroutine and
  a frame ends deep inside the game's code, so the host's C frames on that stack are the guest's call chain. The
  stack is copied as it is: 144-288 bytes at a frame's end. It holds this process's addresses, so it sits in a
  room at the state's start that the desync check leaves out (`vf1net::STACK_ROOM`, `hash_skip`); a rollback only
  ever restores in the same process, with the same code.
- **The match's board** (`src/host/vf1_netplay.*`, `vf1net::Adapter`): a cold start (`model1::power_on`: work RAM
  and more survive a reset) from the host's backup RAM and its GAME ASSIGNMENTS (as values: either side's I/O
  board), free play, the FREE PLAY notices hidden, the cheats off (Random fighter, the secret character, no time
  limit, the ring, Kage's mask, the strands), the I/O board's stand-in, the ranking's names off, the boot run
  unseen.
- **The match's sound**: the board's sound board is taken off it for the match (the main board never hears back
  from it, the same on both sides) and runs on a thread of its own to the real clock, fed the bytes the game sends
  it, a byte every 8 scanlines as the i8251 sends them (main.cpp's `match_sound_start`). Under rollback a frame's
  bytes go once (`Adapter::take_sound`).
- **The score** (`vf1net::Score`): work RAM 501281 and 501282, each player's rounds won (found against the HUD's
  round markers: 13,818 and 13,822 frames of 13,824 alike, the HUD a few frames behind); bits 0x18 of the backup
  RAM's 407483 and 407503, a person playing that side (0x10 alone once they've lost a versus: the CPU takes their
  side, and the loser's asked to press Start for a rematch). After the CPU wins the rounds go back to 0 one at a time, so
  a match's winner is whoever reached more.
- **The names on the energy bars**: Random fighter's own code (`random_fighter_hud`), each player's name spelt in
  the HUD's font in their box, put in the tile map only while a picture's drawn and the game's entries put back
  after (`model1::set_hud_names`): the board never holds them.
- **Drawing never changes the board**: the display list control's bit 0x40 (which list is selected), which the
  game reads back, was brought up to date by the drawing (and by the scan's uploads of a frame not drawn), so a
  frame drawn and the same frame run again unseen came apart. It's brought up to date in one place now, at the
  frame's end, drawn or not (`list_select_settle`); `vf1soak --draw` checks it.

- **Random fighters** (Online > Settings > Your fighter: Normal, Random, a secret character, or the Cheats page's
  seed name; `online_fighter` in settings.ini): the host's goes in its blob with the match's random seed, the
  guest's in its hello (`src/net`'s optional extra: `Identity::extra`, `Game::peer_extra`), so both boards set
  Random fighter up alike before the boot (its records put back and what it knew forgotten first, its moves' patch
  only when someone has one: it changes the game otherwise). Its save state: the ROM's last 512 KB (the records and
  moves tables it builds) and its maps, in key order; the build's flashes, the heads' turns and the poses' easing
  are the picture's. The VS reveal shows each one's fighter under the name, smaller ("RANDOM", "SECRET: SIBA").

- **The character select in lockstep**, as VF3's (`vf1net::in_select`: work RAM 501138, the game's mode, 7 from
  Start to the VS screen, the challenger's select too): no frame on a guess, so a face never flashes as one is put
  right.
- **The flags on the HUD**: two cells left in the name's box (`model1::set_hud_names`), the host drawing the 48 x 36
  flag there on the window's pixels; the box's top and bottom lines under them and under signs ('-') by edged copies
  of those characters in font characters no tile shows, for the picture only (`model1::hud_edges`).

## Fixed on the way

- The TGP's GPIO lines (`mb86233`) were never set, and they're branch conditions: they held whatever the memory
  did. Two boards came apart only when the machine was busy.
- `hide_free_play_text` kept the bytes it replaced in a static: a second board kept its notices.
- `-ffp-contract=off` on every platform (the V60's and the TGP's floating point is game state).

## Rooms and watching (VF3's)

The Online screen is VF3's room browser: the lobby's listed rooms, VF1's (game `vf1`), a row each (its name, host and
flag, Open or the match on with its score, how many watch), open ones first and nearest first, then the matches; All /
Open / Playing; Quick match, Create room and Enter code its buttons, By address and Settings under More. Create room: a
name and Listed in Rooms (off by default: the code only, as ever); every match in it may be watched by anyone with the
code, and the room stays for the next in line. Quick match games are listed too, to watch only (both players, a QUICK
MATCH label, no code to join), when the host's game is a new build (an older one sends nothing: never listed) and both
players have More > Settings > Show my Quick Match games in Rooms On (the default; Off sends `qm_listed:false` with the
search); either way one can be watched by its code, and the browser and Quick match's searching screen say "Quick Match
games can be watched by anyone." 400 watch a match at most; one more is told "Too many watching right now". The
lobby down: one plain line each (Enter code, Quick match, the browser, a watcher after 20 s), the players' match as it
was. The host sends five times a second on a WebSocket to the match's Watch, and each watcher gets the frames on a
WebSocket of its own (`http::Socket`; the HTTPS feed, stream and pieces as the fallbacks); past the lobby's monthly
ceiling the host's sending is answered `off` (it stops, the match goes on) and a new watcher is told the lobby is busy. The
host sends at the pace the lobby says (`every`, 200 ms to 30 s: slow while nobody watches, woken by the first
watcher); a room waits for its player on a WebSocket of its own (`/v1/room/ws`, `/v1/wait` as before if it's refused).
The lobby's off switches (`SWITCH_ROOMS`, `SWITCH_LISTING`, `SWITCH_WATCH`, `SWITCH_WEBSOCKETS`) and an older lobby
are taken as they come: `features` in `/v1/online` says what's on (none: neither rooms nor watching), the browser
then says "Rooms aren't available right now", a watcher "Watching isn't available right now"; room codes and Quick
match work as ever. Every line these screens show is in src/host/online_text.h (whole sentences, `{name}` places: `tr::fill`), for
the translations. The site's Online page opens the game with `vf1recomp://vf1/join/CODE`
and `.../watch/CODE` (src/host/links.cpp: that form only; a Mac's Info.plist, else this user's registry or .desktop
file, written at start). How watching works, the lobby's side and the numbers: VF3's docs/netplay.md and
review/spectate/spectate-design.md; `src/net/watch.*` is VF3's byte for byte.

What's VF1's own:

- **The fighters.** A guest's fighter (Random, a seed's, a secret character) is in its hello, not the host's blob, and a
  watcher has no hello. So the host sends the lobby the session's blob with the guest's fighter after it
  (`vf1net::watch_blob`: the blob, the fighter, its length, "VF1W"); a watcher's adapter (`watching`) takes both from
  it and makes the same fighters from the host's seed as the players' boards. The session's blob and handshake are as
  before: `NET_VERSION` stays v1.5.0 and old and new builds play each other as ever. Without the guest's fighter a
  watcher of a match with one goes out of step at its first check (tried: `vf1net --p2 X3`, frame 94).
- **A late watcher runs the match from its start**, unseen (here ~3,300 frames in a few seconds, as fast as the machine
  allows), as in every game now: no watcher's state is ever handed to another (one could be forged to the players'
  hash). A VF1 state couldn't be anyway: it holds the recompiled code's stack, return addresses of this process's own.
- **Sound.** The match's sound board starts with the watched match's start, so the boot's set-up bytes reach it
  (`Adapter::mute` lets the first frame's through); the frames caught up unseen are silent, at the start and after a
  long spell cut off alike (`Spectator::on_catch_up`); from the first picture on, every frame's bytes as a player's.
- **Old builds.** An old build sends no `watchable`: its rooms are never listed and its matches can't be watched. A new
  host with an old guest is watchable (the host uploads); the guest's fighter comes from its hello, as ever.

Tests: `watchsim` (no ROM; 0, 1 and 50 watchers: the players' every count the same, every watcher frame for frame;
also under ASan/UBSan, and `--lobby http://127.0.0.1:8787` through VF3's lobby, `node tools/lobby/test/serve.mjs
src/index.js 8787` in vf3-recomp); `vf1net --watchers 2 [--watch-late F] [--p1 R --p2 X3]` (the ROM: each watcher's
fighters the players', its board the straight run's at every checkpoint, a late one's checks of every one of the
players' hashes from the start, the players' figures the same as without);
`menutest` (no ROM: every Online page drawn offscreen, `MENUTEST_RENDERER=gpu` for SDL's GPU renderer, the links'
parser, the watch blob); the game: `VF1_WATCH_CODE=XXXXXX` watches at pass 60, `VF1_HOST_PUBLIC=1` lists
`VF1_HOST_ROOM`'s room.

## Where it stands

| Part | State |
| --- | --- |
| Save states | `vf1soak`: every frame rolled back 7 (and 3, 12, only the misses), 20,000 frames identical, the native code against the interpreter; `--draw` 6,000 |
| The adapter, two boards | `vf1net`: in step over 12,000 frames; 90 ms with 5% lost; delay 0; 120 ms with jitter 30; the score the same on all three boards |
| The game online | Two copies through a stand-in lobby: a room code's match in step |
| `src/net` and its tests | VF3's: `nettest` 22, `delaytest`, `netfuzz` 2,000 runs, `roomtest` |
| Watching | `watchsim` 0/1/50 watchers identical for the players; `vf1net --watchers 2` with `R`/`R` and `S1234`/`X3`: both watchers' boards the straight run's at all 19 checkpoints, 120 hashes matched, the players' rollbacks and waits unchanged; three copies of the game through a local lobby (host, guest, a watcher 20 s late) |
| Random fighters | `vf1soak --random-fighters`: 6,000 frames identical; `vf1net --p1 R --p2 S1234`, `--p1 X3 --p2 R` at 90 ms with 3% lost: in step, the score the same |
| To do | A match against another machine (the Mac and the Deck), arm64 against x86-64 (`vf1soak --hashes`) |

```sh
build/bin/vf1soak --frames 20000            # the save state, every frame rolled back
build/bin/vf1soak --draw --frames 6000      # the same, the straight board drawing every frame
build/bin/vf1net --frames 4000              # two boards, 40 ms each way
build/bin/vf1net --p1 R --p2 X3             # with random fighters (R, S<seed>, X<secret>)
build/bin/vf1net --watchers 2 --p1 R --p2 X3   # and two boards watching (one from frame 1500)
build/bin/nettest && build/bin/delaytest && build/bin/netfuzz && build/bin/watchsim && build/bin/wsfuzz
SDL_VIDEO_DRIVER=offscreen build/bin/menutest shots   # the Online pages as pictures, no ROM
```

Tests with the real game: `VF1_HOST_ROOM=1`, `VF1_JOIN_CODE=XXXXXX`, `VF1_RANDOM=1`, `VF1_WATCH_CODE=XXXXXX` (at pass 60,
as the menu's; `VF1_HOST_PUBLIC=1` lists the room),
`VF_LOBBY=http://127.0.0.1:8787` (a lobby on this machine), `VF3_NET_LAG` (the shared library's: a worse network),
`VF1_DUMP_BEGIN=file` (the state a match starts from), `VF_MATCH_SOUND=file` (what the match's sound board played).

The online version (`vf1net::NET_VERSION`): changed only with what a match depends on (the board, the recompiler,
the adapter, the match's settings, the protocol); a test build plays only the same build.
