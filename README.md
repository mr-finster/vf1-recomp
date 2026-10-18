# Model 1 Recomp

This project reimplements Sega's Model 1 arcade hardware as a native desktop runtime. The Model 1 was used by
multiple games; this repository currently supports Virtua Fighter and has experimental Virtua Racing support.

## Games

| Game | Support | Documentation |
| --- | --- | --- |
| Virtua Fighter | Native V60 recompilation; desktop releases for macOS, Windows, and Steam Deck / Linux | [Virtua Fighter guide](src/games/vf/README.md) |
| Virtua Racing | Interpreter build, with an experimental native-recompilation path | [Virtua Racing guide](src/games/vr/README.md) |

Each game requires the player's own MAME ROM set. No game ROMs or ROM-derived program code are included.

## Shared Model 1 implementation

The shared runtime models the Model 1 board around its main NEC V60 CPU: the Fujitsu MB86233 geometry
coprocessor (TGP), polygon rasterizer, System 24 tile layers, sound board, and I/O board. The V60 interpreter,
TGP core, and much of the machine implementation are based on MAME. The SDL 3 host supplies the window, input,
audio, saves, and menus.

The CPU renderer reproduces the arcade's 1x picture. Optional higher render scales, antialiasing, and GPU rendering
are host-side features. On supported systems, the GPU renderer is checked against the CPU renderer and falls back
to the CPU if it detects a difference or error.

The recompiler translates the V60 program at startup using an address-only function map and SLJIT. It reads the
game program from the user's ROM set; ROM bytes are not shipped. The developer-only ahead-of-time path emits C++
from analyzed V60 functions and keeps ROM-derived output under `build/`.

## Build and test

Development requires a C++20 compiler, zlib, SDL 3, and FreeType. On macOS, install the latter dependencies with
`brew install sdl3 freetype`. Virtua Fighter and Virtua Racing have separate build instructions in their guides.
Every game has the same make targets, made from its `src/games/<game>/game.mk`: `<game>`, `<game>-native`,
`<game>-regions`, `<game>-coverage`, `<game>-static`, `<game>-lockstep`, and `<game>-native-lockstep`.

Useful shared checks:

```sh
make test-model1io-adc
make test-vf-profile test-vr-profile test-vr-gameplay test-vr-tgp-track
```

## Repository layout

| Path | Contents |
| --- | --- |
| `src/core` | NEC V60 interpreter and Fujitsu MB86233 (TGP) core |
| `src/machine` | Shared Model 1 memory map, interrupts, TGP, rasterizer, tile chip, sound and I/O boards |
| `src/rt` | Recompiler runtime and startup native recompilation |
| `src/host` | SDL 3 host, input, pacing, and RmlUi menu |
| `src/games/vf`, `src/games/vr` | Title profiles and game-specific behavior |
| `assets/games/vf`, `assets/games/vr` | Each game's address map, icon and other artwork |
| `src/gpu` | Metal and SDL_GPU renderers and shaders |
| `tools/recomp` | Ahead-of-time V60-to-C++ recompiler |
| `tools/mac`, `tools/win`, `tools/linux` | Each platform's `package.sh [game ...]`: the downloads, the same for every game |
| `tools/release` | What the platforms share: game settings (`games.sh`), third-party sources (`deps.sh`), `LICENSES.txt` |
| `tools/games/<game>/release` | A game's download settings (`game.sh`), READMEs and launcher |
| `tools/games/vf/site` | Virtua Fighter's download page (`build.py` builds it into `build/site`) |

## Project notes

- [How it was made and validated](docs/HOW-IT-WAS-MADE.md)
- [Virtua Fighter-specific documentation](src/games/vf/README.md)
- [Virtua Racing-specific documentation](src/games/vr/README.md)

## Credits and licence

The project incorporates work from MAME, Musashi, ymfm, SLJIT, the Z80 core, RmlUi, GLM, stb_image, SDL 3, and
FreeType. `tools/release/licenses.sh` gathers the third-party notices for distribution packages.

The project's own code is MIT-licensed: see [LICENSE](LICENSE). Third-party components retain their respective
licences. This project is not affiliated with or endorsed by Sega.
