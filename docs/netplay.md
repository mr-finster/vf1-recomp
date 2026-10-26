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

## Fixed on the way

- The TGP's GPIO lines (`mb86233`) were never set, and they're branch conditions: they held whatever the memory
  did. Two boards came apart only when the machine was busy.
- `hide_free_play_text` kept the bytes it replaced in a static: a second board kept its notices.
- `-ffp-contract=off` on every platform (the V60's and the TGP's floating point is game state).

## Where it stands

| Part | State |
| --- | --- |
| Save states | `vf1soak`: every frame rolled back 7 (and 3, 12, only the misses), 20,000 frames identical, the native code against the interpreter; `--draw` 6,000 |
| The adapter, two boards | `vf1net`: in step over 12,000 frames; 90 ms with 5% lost; delay 0; 120 ms with jitter 30; the score the same on all three boards |
| The game online | Two copies through a stand-in lobby: a room code's match in step |
| `src/net` and its tests | VF3's: `nettest` 22, `delaytest`, `netfuzz` 2,000 runs, `roomtest` |
| Random fighters | `vf1soak --random-fighters`: 6,000 frames identical; `vf1net --p1 R --p2 S1234`, `--p1 X3 --p2 R` at 90 ms with 3% lost: in step, the score the same |
| To do | The flags on the HUD, a match against another machine (the Mac and the Deck), arm64 against x86-64 (`vf1soak --hashes`) |

```sh
build/bin/vf1soak --frames 20000            # the save state, every frame rolled back
build/bin/vf1soak --draw --frames 6000      # the same, the straight board drawing every frame
build/bin/vf1net --frames 4000              # two boards, 40 ms each way
build/bin/vf1net --p1 R --p2 X3             # with random fighters (R, S<seed>, X<secret>)
build/bin/nettest && build/bin/delaytest && build/bin/netfuzz
```

Tests with the real game: `VF1_HOST_ROOM=1`, `VF1_JOIN_CODE=XXXXXX`, `VF1_RANDOM=1` (at pass 60, as the menu's),
`VF_LOBBY=http://127.0.0.1:8787` (a lobby on this machine), `VF3_NET_LAG` (the shared library's: a worse network),
`VF1_DUMP_BEGIN=file` (the state a match starts from), `VF_MATCH_SOUND=file` (what the match's sound board played).

The online version (`vf1net::NET_VERSION`): changed only with what a match depends on (the board, the recompiler,
the adapter, the match's settings, the protocol); a test build plays only the same build.
