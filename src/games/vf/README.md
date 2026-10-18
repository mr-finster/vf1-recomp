# Virtua Fighter

Virtua Fighter (Sega, 1993) is the primary title supported by this project. Its arcade NEC V60 program is
recompiled to native code at startup, with render scales up to 6x, widescreen, and smooth motion at the display's
refresh rate.

For shared Model 1 architecture, build requirements, and repository information, see the
[root README](../../../README.md). For the port's implementation and validation history, see
[How it was made](../../../docs/HOW-IT-WAS-MADE.md).

> No game files are included. You need your own MAME `vf` set (`vf.zip`). It is read on your computer, and the
> game's code is recompiled there when it starts. This is an unofficial fan project, not made, approved, or
> supported by Sega.

## Status

| Area | State |
| --- | --- |
| Arcade V60 program, recompiled to native code | Working |
| TGP, rasterizer, tile layers, sound and I/O boards | Working |
| Render scale up to 6x, widescreen, ultrawide and smooth motion | Working |
| GPU rendering | Working on macOS (Metal), Windows (Direct3D 12 or Vulkan), and Steam Deck / Linux (Vulkan); checked against the CPU renderer at startup with fallback on errors or differences |
| Sarah's stage lighting | Working |
| Portable mode, custom ROM folder, menu, keyboard and gamepads | Working |
| macOS, Windows, Steam Deck / x86-64 Linux | Working |
| Painted widescreen title and player-select sides | Included in downloads, not in this repository; without them those screens remain 4:3 |
| Online play | Not yet |

## Play

Get the latest release from [GitHub](https://github.com/mr-finster/vf1-recomp/releases/latest) or the
[download site](https://vf1-recomp.pages.dev/):

| System | Download |
| --- | --- |
| macOS 11+ (Apple silicon and Intel) | `VirtuaFighterRecomp-mac.zip` |
| Windows 10 or 11 (64-bit) | `VirtuaFighterRecomp-windows.zip` |
| Steam Deck / x86-64 Linux | `VirtuaFighterRecomp-linux.tar.gz` |

### macOS and Windows

1. Unzip the package. On macOS, move **Virtua Fighter Recomp** to Applications; on Windows, place the
   `VirtuaFighter` folder anywhere except Program Files.
2. Open the app (on Windows, run `VirtuaFighter.exe`).
3. Select your `vf.zip` when prompted. A copy is kept, so this is normally a one-time step.

### Linux and Steam Deck

1. Copy the archive to the Deck and extract it in Desktop Mode. Extracting on Windows or copying through a USB
   stick can lose file permissions and prevent the game from launching.
2. Put `vf.zip` in `VirtuaFighter/roms`.
3. In Desktop Mode, add the `vf` file from the `VirtuaFighter` folder as a Non-Steam Game. Set Start In to that
   folder and leave Steam Play compatibility disabled; it is a native Linux game.
4. Launch it from the Library in Game Mode.

On other x86-64 Linux distributions, extract the archive anywhere, put `vf.zip` in `VirtuaFighter/roms`, and run
`VirtuaFighter.sh`.

The Windows download also runs on the Deck under Proton, but the native Linux package is recommended.

### ROM and data folders

Portable mode keeps settings, saves, screenshots, logs, and ROM copies beside the application. Enable it by placing
an empty `portable.txt` there (beside `VirtuaFighter.exe` on Windows, beside the app on macOS, or in the
`VirtuaFighter` folder on Linux).

To use a ROM folder outside the package, pass `--roms <folder>`, set `M1_ROMS`, or add `roms_folder = <folder>` to
`settings.ini`. The command-line option takes precedence, followed by `M1_ROMS` and then the setting. The game
looks there first for `vf.zip` and optional `model1io.zip`; it reads these files in place. The log identifies the
folder it used. Examples:

```sh
VirtuaFighter.exe --roms "D:\ROMs\vf"
open "Virtua Fighter Recomp.app" --args --roms ~/ROMs/vf
./VirtuaFighter.sh --roms /userdata/roms/ports/vf
```

The macOS and Windows builds are not signed. On macOS, open the app once, then allow it under System Settings >
Privacy & Security. On Windows, choose More info > Run anyway if SmartScreen appears.

## Controls

Controls can be changed in the menu (`Esc`, or View / Select on a pad).

| Key | Action |
| --- | --- |
| Arrow keys | Player 1 movement |
| `Z`, `X`, `C` | Player 1 guard, punch, kick |
| `1` | Player 1 start |
| `I`, `J`, `K`, `L` | Player 2 movement |
| `B`, `N`, `M` | Player 2 guard, punch, kick |
| `2` | Player 2 start |
| `Esc` | Menu: display, sound, game, controls, cheats, quit |
| `P` | Pause |
| `F3` | Reset |
| `F5` / `F6` | Render scale / picture shape |
| `F9` | Frame timings |
| `F11` | Fullscreen |
| `F12` | Screenshot |

On an Xbox-layout gamepad: d-pad or stick moves, X guards, A punches, B kicks, Y guards + punches (throws), RB
punches + kicks, LB guards + kicks, RT performs all three, and Start starts. View (tap), L3 + R3, or touchpad
click opens the menu; View + LB / RB changes render scale / picture shape. In Arcade mode, assign Coin in Controls.

## Secret characters

Menu > Cheats > **Secret character** puts a cut fighter in Akira's player-select spot: Siba (Majido), Jeff, Takeru
(an early Akira), the Koskol brothers Rekka, Score and Aeon, or Dural. They were unfinished, with borrowed moves
and no throws.

**Portraits and icons made from the game's own models.** Only Siba had select art. The others now have portraits
and icons drawn from their real 3D heads using the game's renderer:

- **Portraits:** The select screen draws a live head in a per-character camera. Missing portraits were made from
  each fighter head, adjusting camera and framing to match the existing cast; Dural was turned to face the same
  direction.
- **Icons:** The select row uses flat pixel art. Each new icon was rendered from the character head, framed and
  scaled to match Akira's icon, then reduced to 12 colours. While a secret character occupies Akira's spot, the
  game's tiles for Akira's icon are temporarily swapped. Tools are in `tools/games/vf/select_icons`; generated icons go in
  `assets/games/vf/select_icons` and ship with downloads, but are not stored in this repository.

<p align="center">
  <img src="https://github.com/user-attachments/assets/fe3dc5a5-1e77-413d-8c21-c08bcd572c8c" alt="The secret characters in action at 3x: Dural, Siba, Jeff, Takeru and Rekka on five stages, and Jeff on the player select" width="800">
</p>
<p align="center">
  <img src="https://github.com/user-attachments/assets/eb2039b9-087b-4d73-8241-af9e0a29efda" alt="The cast's select portraits, and the secret characters' below" width="800">
</p>

| Character | Select screen | Health bar | Notes |
| --- | --- | --- | --- |
| Siba (Majido) | Own portrait, icon and profile; MAJIDO (P1) / SHIIBA (P2) | MAJIDO | Most complete. His pounce uses Lau's move; Siba's own had no jump data. |
| Jeff | Own portrait and profile, made icon; JEFFERY | JEFF | Jacky's moves; hat and shades can be knocked off. |
| Takeru | Akira's name and profile, made icon | AKIRA | Early Akira design, in Akira's headband. |
| Rekka | Made portrait and icon; Akira's name and profile | DURAL | Rekka and Score share a head. |
| Score | Made portrait and icon; Akira's name and profile | DURAL | Looks like Rekka. |
| Aeon | Made portrait and icon; Akira's name and profile | DURAL | |
| Dural | Made portrait and icon; Akira's name and profile | DURAL | |

They replace Akira, including arcade-mode CPU Akira, so Akira cannot be selected while one is active. Other cheats
include infinite energy, one-hit KO, no time limit, and a tiny or huge ring.

### Background

Based on research from [The Cutting Room Floor](https://tcrf.net/Virtua_Fighter_(Arcade)) and mountainmanjed's
MAME cheats:

- **Siba (Majido Ab Dul):** Seen in prerelease screenshots and on international cabinet artwork as “Shiiba”
  (sometimes mislabelled Akira). Renamed Majido late in development; the only cut fighter with a select icon and
  recorded voice clips. His moves are borrowed from Sarah and Lau. He returned as Siba in *Fighters Megamix*.
- **Jeff (Jeffery Buckman):** A soldier in camouflage, cap and shades. His name may honor VF1 staff member Jeffrey
  Buchanan; “Jeffery” later went to Jeffry, and the rankings call Jeff WILLIE after a win. He resembles Tekken 2's
  Prototype Jack, from many of the same designers.
- **Takeru (Takeru Ryushoji):** Listed before Akira in the character table and fights under Akira's name, likely an
  early Akira design. His unused bio and nameplate are his own; the rankings say TAKERU after a win.
- **Rekka, Score and Aeon:** Listed after Akira with Dural's name and moves. Early design documents identify them
  as three final bosses, perhaps meant to be fought one per round. They share moves and IDs.
- **Dural:** The actual final boss, not selectable in the arcade. An unused select screen in the code includes a
  portrait of her (and one Koskol brother).

## Development

The V60 program is recompiled from the user's `vf.zip` at startup, guided by the address-only map
`assets/games/vf/vf_map.txt`. No ROM bytes are included. For the shared Model 1 design and developer workflows, see
the [root README](../../../README.md) and [development history](../../../docs/HOW-IT-WAS-MADE.md).

## Credits

Thanks to testers [svera](https://github.com/svera) (portable mode and unused characters),
[terryarcade](https://github.com/terryarcade), [chocolatelily-0716](https://github.com/chocolatelily-0716),
[vmicka72](https://github.com/vmicka72), and [aadsasdasd](https://github.com/aadsasdasd).

Virtua Fighter is Sega's. This project is not affiliated with or endorsed by Sega.
