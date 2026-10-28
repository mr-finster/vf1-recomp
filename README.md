<p align="center"><img src="assets/icon/icon.png" alt="Virtua Fighter Recomp" width="160"></p>

<h1 align="center">Virtua Fighter Recomp</h1>

<p align="center"><b>Virtua Fighter (SEGA, arcade, Model 1, 1993) as a native PC game: the arcade's own V60 code,
recompiled to native code, with render scales up to 6x, widescreen and smooth motion at any refresh rate.</b></p>

<p align="center">
  <a href="https://github.com/mr-finster/vf1-recomp/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/mr-finster/vf1-recomp?label=release"></a>
  <a href="LICENSE"><img alt="Licence: MIT" src="https://img.shields.io/badge/licence-MIT-blue"></a>
  <img alt="Platforms" src="https://img.shields.io/badge/platforms-macOS%20%7C%20Windows%20%7C%20Steam%20Deck%20%2F%20Linux-informational">
</p>

<p align="center">
  <a href="https://vf1-recomp.pages.dev/">Play</a> ·
  <a href="https://github.com/mr-finster/vf1-recomp/releases/latest">Download</a> ·
  <a href="https://youtu.be/LNuS-F0WIUc">Trailer</a> ·
  <a href="https://youtu.be/yranppHYl7A">Secret characters</a> ·
  <a href="#status">Status</a> ·
  <a href="#controls">Controls</a> ·
  <a href="https://github.com/mr-finster/vf1-recomp/issues">Report a bug</a>
</p>

<p align="center">
  <a href="https://vf1-recomp.pages.dev/img/montage.jpg?v=2"><img src="https://vf1-recomp.pages.dev/img/montage.jpg?v=2" alt="Virtua Fighter Recomp at 3x: fights on five stages and the widescreen player select" width="800"></a>
</p>

<p align="center">
  <a href="https://youtu.be/LNuS-F0WIUc"><img src="https://img.youtube.com/vi/LNuS-F0WIUc/maxresdefault.jpg" alt="Watch the online trailer" width="395"></a>
  <a href="https://youtu.be/yranppHYl7A"><img src="https://img.youtube.com/vi/yranppHYl7A/maxresdefault.jpg" alt="Watch the secret characters trailer" width="395"></a>
  <br><sub>The online trailer · Secret characters (click to watch on YouTube)</sub>
</p>

> [!IMPORTANT]
> No game files are included: no ROMs and nothing made from them. You need your own dump of the game as MAME's
> `vf` set (`vf.zip`). It is read on your computer, and the game's code is recompiled there when it starts.
> An unofficial fan project, not made, approved or supported by SEGA, and not affiliated with it.

## About

Virtua Fighter Recomp runs the arcade game's own NEC V60 program, recompiled to native code on your machine from
your `vf.zip`, and reimplements the rest of the Model 1 board around it: the TGP geometry coprocessor, the polygon
rasterizer, the tile layers, the sound board and the I/O board, from MAME's work on the Model 1. At 1x you get the
arcade's frames pixel for pixel; above it, render scales up to 6x, widescreen and ultrawide, and smooth motion at
your screen's refresh rate.

## Status

| Area | State |
| --- | --- |
| The arcade's V60 program, recompiled to native code | Working |
| TGP, rasterizer, tile layers, sound and I/O boards | Working: reimplemented from MAME's Model 1 code |
| Render scale up to 6x, widescreen and ultrawide, smooth motion | Working |
| The 3D drawn on the GPU (Display > 3D renderer: Auto, GPU or CPU) | Working on macOS (Metal), Windows (Direct3D 12 or Vulkan) and Steam Deck / Linux (Vulkan): the same picture as the CPU's, checked when the game starts; Auto uses the GPU whenever that check passes (not a software GPU such as llvmpipe), and any difference or error falls back to the CPU |
| Sarah's stage lighting (the light from below pulsing with the floor) | Working, as on the cabinet |
| Portable mode, your own ROM folder | Working |
| Menu, keyboard and gamepads, two players | Working |
| macOS, Windows, Steam Deck / x86-64 Linux | Working |
| Painted widescreen sides for the title and player select | Working in the downloads; not in this repository (4:3 without them) |
| Random fighters, secret characters and cheats (menu > Cheats) | Working: see below |
| The game's language (menu > Arcade settings: English or Japanese) | Working; English from the first start |
| Online play | Experimental: rollback netplay, a random opponent or a friend by room code, no setup, random fighters online (see [Online play](#online-play-experimental)) |

## Random fighters

<p align="center">
  <a href="https://youtu.be/9H62laX0VAI"><img src="https://img.youtube.com/vi/9H62laX0VAI/maxresdefault.jpg" alt="Watch: Virtua Fighter 1 But Every Character Is Randomized" width="800"></a>
</p>
<p align="center">
  <img src="https://github.com/user-attachments/assets/c3c5aa9a-f720-4338-aec1-52656214b68c" alt="Random fighters: a lineup of them over the game" width="800">
</p>

Menu > Cheats > **Player 1 fighter** (and **Player 2 fighter**):

- **Random:** a new fighter every game, its head, body and limbs from three of the cast and its moves dealt from the
  same three, so it plays like a mix of them, at the real game's timings. Its voice
  is one of the three's, and it gets a name made of theirs on the energy bar (JACPAIWOLF, WOLFLAURAH).
- **Seed:** type a fighter's name, as seen on an energy bar, to get that fighter again: the same build and the same
  moves on any machine. The colours are new every game.
- **Secret character:** one of the fighters cut from the game, below.

Anything but Normal is picked for you at the select. Round 1 builds each random fighter from its polygons as the fight
starts. **Random opponents** makes every computer opponent a random fighter, made afresh for each match.

## Secret characters

Menu > Cheats > **Player 1 fighter** (or **Player 2 fighter**) > **Secret character**, then **Character**: a fighter
cut from the arcade ROM, picked for you at the select: Siba (Majido), Jeff, Takeru (the early Akira), the Koskol
brothers Rekka, Score and Aeon, or Dural. They were never finished (borrowed moves, no throws), so expect oddities.
The other player and the computer keep the real Akira.

**Portraits made from the game's own models.** Only Siba ever got select art; the rest had none, so the
select screen showed Akira's (or nothing). Each one now has a portrait drawn from its real 3D head in the
ROM, by the game's own renderer:

- **Portraits:** the game's select screen draws a live 3D head in a camera of its own per character. The ones without
  a portrait get one built from their fight head, and each has its own camera, found by rendering every head against
  the cast's and measuring: the eyes at the same height, the face in the middle, the head the same size, the neck off
  the bottom. Dural's is also turned to face the way the others do.

<p align="center">
  <img src="https://github.com/user-attachments/assets/fe3dc5a5-1e77-413d-8c21-c08bcd572c8c" alt="The secret characters in action at 3x: Dural, Siba, Jeff, Takeru and Rekka on five stages, and Jeff on the player select" width="800">
</p>
<p align="center">
  <img src="https://github.com/user-attachments/assets/eb2039b9-087b-4d73-8241-af9e0a29efda" alt="The cast's select portraits, and the secret characters' below" width="800">
</p>

| Character | Select screen | Health bar | Notes |
| --- | --- | --- | --- |
| Siba (Majido) | Own portrait and profile; named MAJIDO (P1) or SHIIBA (P2) | SIBA | The most complete of them. His pounce is Lau's: his own had no jump in its data and flew him across the ring |
| Jeff | Own portrait and profile; named JEFFERY | JEFF | A handful of Jacky's moves; his hat and shades can be knocked off |
| Takeru | Own portrait and profile (as Akira Ryuzaki, his early name) | TAKERU | The early Akira, in Akira's headband |
| Rekka | Made portrait (no blinking) | REKKA | Rekka and Score share a head |
| Score | Made portrait (no blinking) | SCORE | Looks like Rekka |
| Aeon | Made portrait (no blinking) | AEON | |
| Dural | Made portrait (no blinking) | DURAL | |

The rest of Cheats: Kage's mask (a hit can knock his hood back, as on a cabinet played for nine hours), infinite
energy (player 1, 2 or both), one-hit KO, no time limit, and a tiny or huge ring.

### Who they were

From [The Cutting Room Floor](https://tcrf.net/Virtua_Fighter_(Arcade)), whose research (and mountainmanjed's MAME
cheats) these come from:

- **Siba (Majido Ab Dul)**: seen in pre-release screenshots and on the international cabinet's artwork, as "Shiiba"
  (on some cabinets mislabelled Akira). Renamed Majido before he was cut, late: he is the only one with his own select
  icon and voice clips, recorded like the final cast's. His moves are borrowed from Sarah and Lau. He came back as a
  hidden character, Siba, in Fighters Megamix.
- **Jeff (Jeffery Buckman)**: a soldier in camo, a cap and shades. His name is likely a tribute to VF1 staff member
  Jeffrey Buchanan, and "Jeffery" later went to Jeffry; finish the game with him and the rankings call him WILLIE,
  Jeffry's early name. He looks remarkably like Tekken 2's Prototype Jack, from many of the same designers.
- **Takeru (Takeru Ryushoji)**: listed just before Akira in the game's character table and fighting under Akira's name,
  most likely Akira's early design, but with his own unused bio and nameplate, and TAKERU on the rankings when you
  finish with him. (Akira himself was once Akira Ryuzaki.)
- **The Koskol brothers, Rekka, Score and Aeon**: listed just after Akira, with Dural's name and moves. Early design
  documents name them as the game's three final bosses, perhaps meant to be fought a round each; with the same moves
  and IDs, they're in effect one character with three outfits.
- **Dural**: the game's real final boss, never selectable. An early, unused select screen in the code has a portrait
  for Dural (and one for a Koskol brother), so she may once have been meant to be playable.

## Play

From the [latest release](https://github.com/mr-finster/vf1-recomp/releases/latest), or the
[site](https://vf1-recomp.pages.dev/):

| System | Download |
| --- | --- |
| macOS 11+ (Apple silicon and Intel) | `VirtuaFighterRecomp-mac.zip` |
| Windows 10 or 11 (64-bit) | `VirtuaFighterRecomp-windows.zip` |
| Steam Deck / x86-64 Linux | `VirtuaFighterRecomp-deck.tar.gz` |
| Linux on ARM (ARM64: a Raspberry Pi 5; Ubuntu 22.04, Debian 12, Raspberry Pi OS bookworm or newer), unpacked and run as the x86-64 Linux one | `VirtuaFighterRecomp-linux-arm64.tar.gz` |

**Mac and Windows**

1. Unzip it (Mac: move **Virtua Fighter Recomp** to Applications; Windows: the VirtuaFighter folder anywhere, not inside Program Files).
2. Open it (Windows: `VirtuaFighter.exe`).
3. Choose your `vf.zip` when it asks. A copy is kept, so you only do this once.

**Steam Deck**

1. Copy the download to the Deck (e.g. Downloads) and unpack it there: in Desktop Mode, right-click it in the file
   manager > Extract > Extract archive here. (Unpacking on Windows, or copying the folder through a USB stick, can
   lose the files' permissions, and then nothing launches.)
2. Put your `vf.zip` in the `VirtuaFighter/roms` folder.
3. Desktop Mode: Steam > Games > Add a Non-Steam Game to My Library > Browse, set the file type filter to "All
   files", pick the `vf` file in the VirtuaFighter folder, Add Selected Programs. In its Properties, set Start In to
   the VirtuaFighter folder (e.g. `/home/deck/Downloads/VirtuaFighter/`), and under Compatibility leave "Force the
   use of a specific Steam Play compatibility tool" off: it's a native Linux game, and Proton stops it launching.
4. Back in Game Mode, launch it from your Library with the default Gamepad layout.

The Windows download also runs on the Deck under Proton (add `VirtuaFighter.exe` instead, with Proton forced on),
but the native Linux build above is the one to use.

**Portable mode:** put a file named `portable.txt` next to the program (Windows: beside `VirtuaFighter.exe`; Mac:
beside the app, in the same folder; Steam Deck / Linux: in the VirtuaFighter folder) and the game keeps everything in
that folder: settings, saves, screenshots, `log.txt` and the copy of your `vf.zip` (in its `roms` folder), with
nothing in your user data folder (`%APPDATA%`, `~/Library/Application Support`, `~/.local/share`).

**Your own ROM folder** (e.g. Batocera's `/userdata/roms/ports/vf`, or any folder an update won't touch): start the
game with `--roms <folder>` (or `--roms=<folder>`), set the environment variable `VF_ROMS`, or add
`roms_folder = <folder>` to `settings.ini`; the command line wins, then `VF_ROMS`, then `settings.ini`. `~` is your home
folder, and a relative path in `settings.ini` is from its folder. The game looks there first for `vf.zip` (and `model1io.zip`, if you have it) and
only reads it (no copy is kept anywhere else); `log.txt` names the folder it used. If the zip isn't there, the log
says where it looked and the game carries on as without the option; a zip you then choose with the file picker is
copied into your ROM folder, so the next start finds it there. Examples: Windows `VirtuaFighter.exe --roms "D:\ROMs\vf"`;
Mac `open "Virtua Fighter Recomp.app" --args --roms ~/ROMs/vf`; Linux / Batocera
`./VirtuaFighter.sh --roms /userdata/roms/ports/vf`.

> [!NOTE]
> The Mac and Windows builds aren't signed. On macOS, open it once, close the message, then System Settings >
> Privacy & Security > Open Anyway. On Windows, if SmartScreen warns, choose More info > Run anyway.

## Controls

Change them in the menu (<kbd>Esc</kbd>, or View / Select on a pad).

| Key | Does |
| --- | --- |
| <kbd>↑</kbd> <kbd>↓</kbd> <kbd>←</kbd> <kbd>→</kbd> | Player 1: move |
| <kbd>Z</kbd> <kbd>X</kbd> <kbd>C</kbd> | Player 1: guard, punch, kick |
| <kbd>1</kbd> | Player 1: start |
| <kbd>I</kbd> <kbd>J</kbd> <kbd>K</kbd> <kbd>L</kbd> | Player 2: move |
| <kbd>B</kbd> <kbd>N</kbd> <kbd>M</kbd> | Player 2: guard, punch, kick |
| <kbd>2</kbd> | Player 2: start |
| <kbd>Esc</kbd> | Menu: display (render scale, picture shape, antialiasing, 3D renderer...), sound, game, controls, cheats (the unused characters), quit |
| <kbd>P</kbd> | Pause |
| <kbd>F3</kbd> | Reset |
| <kbd>F5</kbd> / <kbd>F6</kbd> | Render scale / picture shape |
| <kbd>F9</kbd> | Frame timings |
| <kbd>F11</kbd> | Fullscreen |
| <kbd>F12</kbd> | Screenshot |

Gamepad (Xbox layout): d-pad or stick to move, X guard, A punch, B kick, Y guard + punch (throws), RB punch + kick,
LB guard + kick, RT all three, Start to start. View (tap), L3 + R3 or the touchpad click opens the menu (Menu can be
moved in Controls); View + LB / RB change the render scale / picture shape. In Arcade mode, set Coin in Controls (View
works) for a coin button.

## Online play (experimental)

Rollback netcode, as modern fighting games and Virtua Fighter 3 Recomp have it: find a random opponent in one click, or send a friend a room code. There's nothing to set up and no ports to forward. The game runs at the Model 1 board's own 57.5 fps online too: input delay and rollback absorb the connection, so it never slows down to wait for it.

> **Both players need the same `vf.zip`**, and the same online version: every release from v1.5.0 on plays the others, until one changes the game (its notes will say so). A build of your own between releases plays only the same build. The game checks both, and tells you in plain words if they don't match.

### How to play online

Open the menu (Esc, or View / Select on a controller) and choose **Online**. The first time, it asks for your name (up to 10 letters). Your name shows under your energy bar, with your country's flag.

**RANDOM: play anyone**
- Pick **RANDOM**. The card shows how many people are online right now.
- It finds whoever else is searching, the nearest first (the best connection), and widens the search the longer you wait. **Stop** gives up.
- You can play solo while you wait. The match takes over as soon as someone's found.
- After a match, **New opponent** searches again.

**HOST / JOIN: play a friend**
1. One player picks **HOST**. It shows a 6-letter room code like `FR7-W25`. **Copy code** puts it on the clipboard: send it to your friend.
2. The other picks **JOIN** and types or pastes the code. It joins by itself at the 6th letter, and the steps on screen show how far it's got.
3. Once you're both in, the game starts at its title screen: both press Start.

On a controller, the code and name screens show letters to pick with A (X deletes, Y pastes). On a keyboard, just type.

**Your fighter** (Online > Settings)
- **Normal:** pick at the select, as ever.
- **Random:** a new random fighter at every select: a head, a body and moves mixed from across the cast, as Menu > Cheats makes them.
- **Seed:** one named fighter every game, the same build and moves on any machine. Type its name (from its energy bar, like `SAJACWOLF`) in the **Seed** row under it.
- **Secret:** one of the characters left in the game (Siba, Jeff, Takeru, the Koskol brothers, Dural).
- It's yours whichever side you're on, and your opponent sees it under your name on the VS screen before the fight. Your own Cheats stay off in a match: both sides play the same game.

**In a match**
- Each player uses their own **Player 1** controls.
- Matches play the PC way (free play), with the host's arcade settings (rounds, difficulty, energy).
- The stage is random, like a cabinet that's been on all day: a random one for the first fight, then the game's own order.
- Whoever loses is asked to press Start for a rematch. As in the arcade, the winner plays on against the computer until they do.
- After leaving, **Play again** reopens the same room code, and your friend rejoins by itself.

### How online play works

- **Rollback netcode**, like modern fighting games and Fightcade. Your own inputs show at once, after a small input delay. When your opponent's inputs arrive, the game quietly re-runs the last few frames with them, so it never waits on the network. At higher ping you may see a move "jump" slightly as it's corrected: that's rollback doing its job.
- **Auto input delay**, on by default:
  - It uses as little delay as it can: none on a LAN and 1 frame almost anywhere else, with rollback quietly covering the rest.
  - **It accounts for slower machines.** Each game measures how many frames it can re-run without a hitch and tells the other; the other side's delay covers the rest, so a Steam Deck in a heavy fight never stutters.
  - Against a far opponent it adds delay rather than slowing the game down, so the game always runs at its full 57.5 fps (about 3 frames at 170 ms, 5 at 250 ms, up to 15).
  - It ignores short bursts (a phone network's second of high ping): rollback covers those.
  - You can set it yourself instead (0–10) in Online > Settings.
- **Keeping in time.** When one side runs a frame or two ahead of the other, it slows down a touch (2% a frame) until the other catches up, rather than stopping for a whole frame. So a match runs at the pace both machines can hold: 57.5 fps when both can.
- **The sound runs on its own clock.** In a match, the music, voices and effects play in real time on their own, so a slow frame, a wait for the other player, or the game keeping in time never makes them drop out or change pitch. The Sound page's volumes apply mid-match.
- **Direct first, relay if needed.** The two games connect to each other directly whenever they can. When a network won't allow that (phones' 5G, some office, school and hotel networks), the match goes through Cloudflare's relay instead, automatically. The connection display says **DIRECT** or **RELAY**.
- **The lobby only introduces players.** Room codes and random matching run on a small Cloudflare service. No game data goes through it, and it doesn't log anything.
- **Desync protection.** Both games check every 2 seconds that they're in exactly the same state, on a Mac against a PC too. If they ever weren't, the match ends with a message rather than playing on wrong.

### The connection display

At the bottom of the screen during a match, out of the way of the game's own bars and timer:

| | |
|---|---|
| **● NAME** | Your opponent. The dot is green (great), yellow (OK) or red (rough), by how the match plays. |
| **PING** | Round trip in milliseconds. Under ~100 ms plays great; up to ~250 ms plays well. |
| **DELAY** | Your input delay in frames (Auto changes it as needed). |
| **RB** | The most frames rolled back recently. |
| **DIRECT / RELAY** | How you're connected. |

Online > Settings > **Connection display** sets it to On, Detailed or Off. **Detailed** adds the jitter, the average rollback, how far ahead your side is running, and what each frame costs your machine.

### Privacy

- **IP addresses:** in a direct match your opponent's game knows your IP address, as in any peer-to-peer game.
  - A random opponent never gets your home network's local address.
  - **Hide my IP address** (Online > Settings) sends every match through the relay, so your opponent only sees Cloudflare. It adds a little ping.
- **Your flag** is the country Cloudflare places your connection in. A VPN shows the VPN's country.
- **log.txt** masks every IP address, so it's safe to post.

### Known issues

- **Few players online at first.** RANDOM only finds players searching at the same time, so tell a friend, or post a time to meet.
- **Each game plays only itself.** A Virtua Fighter player can't match a Virtua Fighter 2 or 3 one.
- **Far opponents:** past about a 250 ms round trip, Auto adds input delay to keep full speed. Beyond about 670 ms the game slows down instead.
- **Slow machines:** a machine that can't hold full speed offline will struggle online, and slows the match for both. Use the GPU 3D renderer (see Troubleshooting).
- **Through the relay** the ping is a little higher than direct, and a session through it may need to reconnect after 6 hours.
- **A dropped connection** holds the match for a few seconds, then ends it if it doesn't come back.
- **The menu doesn't pause** an online match. It goes on behind the menu, with your inputs off.
- **The character select plays on guesses**, like the fight: for a moment you may see the other player's cursor or face jump as their inputs arrive.
- **Mac:** the app isn't signed, so macOS may ask to allow incoming connections again for each new version.
- **Not yet:** spectating, replays, lobbies or chat.

### Troubleshooting

**"Couldn't reach the online service"**
- Check your internet connection.
- A firewall or antivirus may be blocking the game: allow it (see Firewalls below).
- If it's the service itself, try again in a few minutes.

**"You both need the same version"**: one of you has a test build or one built from source between releases (those only play the same build), or a later release changed the game. Both download the latest.

**"Your game files (the ROM) aren't the same"**: you have different `vf.zip` files. Both need the same dump.

**It won't connect, or gets stuck on a step**
- The Online screen shows which step it reached and why, in plain words.
- Check the code, and that the host is still hosting: a room closes when the host stops.
- **Try again** goes again. If a direct connection can't be made, it uses the relay by itself.

**Firewalls**
- **Windows:** the first time you host, allow the game when asked. If you dismissed it: Windows Security > Firewall > Allow an app, and tick Virtua Fighter Recomp for **Private and Public**.
- **Mac:** System Settings > Network > Firewall > Options: allow it. Also System Settings > Privacy & Security > Local Network: on.

**Stutter, slowdown, or lots of rollback**
- **Use the GPU renderer:** Menu > Display > 3D renderer: **GPU**. On Windows and Linux, Vulkan or Direct3D 12 work best.
- **Check your speed offline:** Display > Show FPS should read 57 or 58 steadily. If it doesn't offline, it won't online. Lower the render scale (F5).
- **Use a cable** if you can: Wi-Fi adds jitter.
- **Steam Deck:** turn off Wi-Fi power management (Settings > System > Developer Mode on, then Settings > Developer > Enable Wi-Fi power management: off).
- Set the Connection display to **Detailed**: a high JITTER means the network; a high FRAME means the machine.

**"The match went out of step"** means the two games disagreed: a desync. This shouldn't happen. Please report it with **both** players' `log.txt`.

### Reporting a problem

[Open an issue on GitHub](https://github.com/mr-finster/vf1-recomp/issues) with:
1. **Both players' `log.txt`** if you can (one helps too). The last 3 runs are kept as `log.1.txt` to `log.3.txt`. They're in the game's data folder:
   - **Windows:** `%APPDATA%\Virtua Fighter Recomp`
   - **Mac:** `~/Library/Application Support/Virtua Fighter Recomp`
   - **Steam Deck / Linux:** the game's own folder, or `~/.local/share/Virtua Fighter Recomp`
2. What each player is on (PC/Mac/Deck), Wi-Fi or cable, and roughly where (country is enough).
3. The ping and connection shown, and what happened.

## How it works

When it starts, the game's V60 program from your `vf.zip` is recompiled to native code with SLJIT
(`src/rt/native.cpp`), guided by `assets/vf_map.txt`: a list of where the program's functions start, addresses only,
no bytes from the ROM. Around it runs a reimplementation of the Model 1 board: memory map and interrupts, the
Fujitsu MB86233 TGP, the rasterizer, the System 24 tile chip, the sound board (68000, MultiPCM, YM3438) and the I/O
board, all from MAME's code. An SDL 3 host draws the picture at any scale and shape and fills in the frames between
the arcade's for smooth motion.

The 3D can also be drawn on the GPU (`src/gpu`): Metal on a Mac, and SDL_GPU compute (Direct3D 12 or Vulkan) on
Windows and Linux. The GPU doesn't use the usual depth buffer: the polygons are drawn in the game's own order, the
order standing in for depth, by a compute rasteriser that follows the CPU renderer's arithmetic step for step
(integer division, no fused multiply-adds, float-to-integer conversion as on x86), so its picture is the CPU's pixel
for pixel. When the game starts it draws a test scene both ways and compares them, then checks its first pictures of
the game itself; any difference, error or missing driver and it uses the CPU for that run. Auto (the default)
then picks the GPU unless it's clearly slower than the CPU on that machine.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/core` | the NEC V60 interpreter and the Fujitsu MB86233 (TGP) core, from MAME |
| `src/machine` | Model 1: memory map, interrupts, TGP, rasterizer, tile chip, sound and I/O boards, smooth motion |
| `src/rt` | the recompiler's runtime, and the recompile done on the player's machine (SLJIT) |
| `src/host` | the SDL 3 host: window, input, pacing, the RmlUi menu |
| `src/gpu` | the GPU renderer for the 3D: Metal (`gpu3d_metal.mm`), SDL_GPU (`gpu3d_sdlgpu.cpp`), its HLSL compute shaders (`gpu3d.hlsl`) and their compiled SPIR-V / DXIL (`gpu3d_shaders.h`) |
| `tools/gpu/shaders.sh` | compiles `gpu3d.hlsl` into `gpu3d_shaders.h` with DXC |
| `assets/` | `vf_map.txt` (function addresses), the app icon, the menu (`ui/`) and its font |
| `third_party/` | MAME code, Musashi, ymfm, SLJIT, the Z80 core, RmlUi, GLM, stb_image |
| `tools/recomp` | the ahead-of-time recompiler (V60 to C++), for development |
| `tools/port` | the script that ported MAME's TGP and rasterizer |
| `tools/*.cpp`, `tools/romset.py` | region builder, disassembler, headless runner, lockstep checker, coverage |
| `tools/mac`, `tools/win`, `tools/deck`, `tools/release` | the release packages and their licence notices |
| `tools/site` | the download site |

## Run it yourself

Needs a C++20 compiler, zlib, SDL 3 and FreeType (Mac: `brew install sdl3 freetype`).

```
make native            # build/bin/vf-native: the game, recompiled from your vf.zip when it starts
build/bin/vf-native
```

The release packages: `tools/mac/package.sh` (Mac, universal), `tools/win/build.sh` (Windows, cross-compiled with
MinGW-w64), `tools/deck/build.sh` (Steam Deck / x86-64 Linux, in Valve's Steam Runtime "sniper" SDK container).
They also take `assets/backgrounds/` (painted widescreen sides for the title and player select screens), which
isn't in this repository: the game runs without it, and those two screens are then shown 4:3.

For development, the V60 code can also be recompiled ahead of time to C++ (kept in `build/`, never committed):
`python3 tools/romset.py roms/vf.zip build/vf roms/model1io.zip`, then `make` builds `build/bin/vf`. `make rec`
builds `vflock`, which runs MAME's V60 interpreter and the recompiled code side by side and compares every register
at every scanline.

## How it's made

Made with the help of AI coding tools. Every build is checked against the arcade: the recompiled code runs side by
side with an interpreter of the original code, and both are compared frame by frame with MAME's pictures, RAM and
sound.

The whole story, with how the AI-assisted work was organised and checked: [docs/HOW-IT-WAS-MADE.md](docs/HOW-IT-WAS-MADE.md).

## Credits

Built on MAME's work on the Model 1: the V60 core and disassembler (Farfetch'd, R. Belmont), the TGP, the
rasterizer, the System 24 tile chip and the machine (Olivier Galibert), the MultiPCM (Miguel Angel Horna) and the
sound board (R. Belmont), all BSD-3-Clause. Also Musashi (Karl Stenerud, MIT), ymfm (Aaron Giles, BSD-3-Clause),
SLJIT (Zoltan Herczeg, BSD-2-Clause), the Z80 core (Nicolas Allemand, MIT), RmlUi (MIT), GLM (MIT), stb_image
(Sean Barrett, public domain / MIT), SDL 3 (zlib), FreeType (FreeType License) and the Barlow Condensed font (SIL
Open Font License). `tools/release/licenses.sh` gathers their notices for the downloads.

Thanks to everyone who tested and reported: [svera](https://github.com/svera) (portable mode, the unused
characters), [terryarcade](https://github.com/terryarcade), [chocolatelily-0716](https://github.com/chocolatelily-0716),
[vmicka72](https://github.com/vmicka72) and [aadsasdasd](https://github.com/aadsasdasd). The secret characters come
from mountainmanjed's MAME cheats, documented on [The Cutting Room Floor](https://tcrf.net/Virtua_Fighter_(Arcade)).

Virtua Fighter is SEGA's. This project is not affiliated with or endorsed by SEGA.

## Licence

MIT for this project's own code: see [LICENSE](LICENSE). Code from MAME keeps its BSD-3-Clause licence and notices;
the rest of `third_party/` keeps its own.

## Support

If you enjoy it, you can [buy me a coffee](https://buymeacoffee.com/mrfinster).
