# Virtua Racing

Virtua Racing (Sega, 1992) is supported as an experimental second Model 1 title. The desktop build currently
runs on the V60 interpreter by default; an address map and separate native-recompiled build are available for
development and testing.

For shared Model 1 architecture, build requirements, and repository information, see the
[root README](../../../README.md). For the port's implementation and validation history, see
[How it was made](../../../docs/HOW-IT-WAS-MADE.md).

> No game files are included. You need your own MAME `vr` set (`vr.zip`) and the Model 1 I/O-board ROM
> (`model1io.zip`). The game is an unofficial fan project,
> not made, approved, or supported by Sega.

## Run Virtua Racing

Build and run the desktop host:

```sh
make vr
build/bin/vr
```

Like Virtua Fighter, the VR host reads your MAME set itself when it starts: on first launch it asks for `vr.zip`,
checks every ROM by CRC, and copies the zip into the game's user-data folder so it only asks once. `--roms <folder>`,
`M1_ROMS`, or `roms_folder` in `settings.ini` point it at a ROM folder of your own instead. With `roms/vr.zip` in the
repository, `make vr` also assembles development regions under `build/vr/` (with `tools/romset.py`), which the host
uses when run from the repository root. The VR host has settings and saves separate from Virtua Fighter.

You also need the common Model 1 I/O-board ROM: put `model1io.zip` next to `vr.zip`; a picked `vr.zip` brings a
`model1io.zip` beside it along. The steering wheel and both pedals go through that board's analog inputs, so without
it the game starts but can't be steered or accelerated: a stand-in board answers only the digital inputs (shift,
start, coin).

Release packages include no ROMs and need no preparation step or Python. They are produced for macOS
(`VirtuaRacingRecomp-mac.zip`), Windows (`VirtuaRacingRecomp-windows.zip`), and Steam Deck / x86-64 Linux
(`VirtuaRacingRecomp-linux.tar.gz`, which reads `vr.zip` from its `roms` folder, as there is no file picker in Game Mode).

## Controls and display

Open the menu with `Esc` or View / Select, then use Controls to configure Player 1. On a keyboard, Left/Right
steer, Up accelerates, Down brakes, and X/Z shift up/down. On a gamepad, the defaults are left stick X for
steering, right trigger for accelerator, and left trigger for brake. Digital controls also include Start and Coin.

The VR I/O profile defines digital labels and bit positions, analog channel names, neutral values, and host-axis
defaults. If `model1io.zip` is installed, the headless runner accepts analog values from 0 to 255:

```sh
VR_WHEEL=128 VR_ACCEL=48 VR_BRAKE=48 build/bin/model1run --game vr build/vr 5200
```

The headless tools are shared with VF: `--game vr` (or a regions folder named `vr`) picks the game. The runner
also supports:

- `M1_SCALE=1..12` for render scale
- `M1_ASPECT=1.3333..3.5556` for picture shape
- `M1_AA=0..3` for edge smoothing or supersampling

VR widens active driving scenes and keeps the track-selection screen and camera-less screens at 4:3. It does not
use Virtua Fighter's camera IDs or title/select backdrop rules.

## Experimental native recompilation

The address-only map at `assets/games/vr/vr_map.txt` is generated from scripted coverage and contains no ROM bytes.
Build the separate native executable with:

```sh
make vr-native
```

Run it with `./build/bin/vr-native`; it recompiles the game from the `vr.zip` it loads at launch. With your own
`roms/vr.zip` in the repository, this also assembles the local regions under `build/vr/`, which it uses when run from
the repository root. No ROM data is included in the executable.

`make test-romset` checks the launch-time loader against `tools/romset.py`: every region built from your local
`roms/vf.zip` and `roms/vr.zip` must match the Python tool's byte for byte.

Run the strict interpreter/native comparison across Beginner and Expert scenarios with:

```sh
make vr-native-lockstep
```

The checks compare CPU registers, RAM, and rendered frames, require no interpreter fallbacks, and assert progress
through track selection, the starting grid, active racing, and course results. The candidate map passes these
scenarios, but coverage is not exhaustive release validation. The regular `make vr` target remains
interpreter-based while more gameplay paths are explored.

To regenerate coverage (`build/cov/vr/`) and the candidate map (`build/gen/vr_map.txt`), run `make vr-static`;
generated artifacts stay in `build/`. `make vr-lockstep` checks the generated C++ against the interpreter. The
scenarios and frame counts are in `src/games/vr/game.mk` (`make vr-lockstep vr_LOCK_FRAMES=...` overrides one). Set
`M1_TGP_TRACE=1` with the headless runner to report whether the TGP ROM-backed core or HLE is active and how many
MB86233 cycles ran.

To check the shared ADC implementation, run `make test-model1io-adc`. With the optional I/O-board ROM, compare
analog controls using two runs:

```sh
mkdir -p build/vr_idle build/vr_full
M1_ASPECT=1.7778 M1_SCALE=2 M1_AA=2 M1_SCRIPT=1 VR_WHEEL=128 VR_ACCEL=48 VR_BRAKE=48 build/bin/model1run --game vr build/vr 5200 500 build/vr_idle
M1_ASPECT=1.7778 M1_SCALE=2 M1_AA=2 M1_SCRIPT=1 VR_WHEEL=128 VR_ACCEL=255 VR_BRAKE=48 build/bin/model1run --game vr build/vr 5200 500 build/vr_full
```

Compare `f05000.png` in each directory, then repeat while changing only `VR_WHEEL` or `VR_BRAKE`. These analog
checks are interpreter-only and require `model1io.zip`.
