VIRTUA RACING RECOMP (preview)
==============================

An unofficial fan project, not made, approved or supported by SEGA.
No game files are included. You need your own MAME "vr" set (vr.zip).

FIRST LAUNCH
Start Virtua Racing Recomp. It asks for your vr.zip, checks it and copies it into its user-data
folder, so you only do this once. If model1io.zip (the Model 1 I/O board's program) is next to
your vr.zip, it is copied too and enables the real analog I/O board.

On Steam Deck / Linux, put vr.zip (and model1io.zip, if you have it) in the package's roms
folder instead: Game Mode has no file picker. Then add VirtuaRacing.sh as a Non-Steam Game and set
Start In to the extracted VirtuaRacing folder.

Keep your ROMs in a folder of your own? Add  roms_folder = <your folder>  to settings.ini, or
start the game with  --roms "<your folder>".

The macOS preview is not notarized; if Gatekeeper blocks it, Control-click the app, choose Open,
then confirm. Windows may show SmartScreen for the same reason.

CONTROLS
Open the menu with Esc or View/Select, then Controls > Player 1. Keyboard: Left/Right steer,
Up accelerates, Down brakes, X/Z shift up/down. Gamepad: left stick X steers, right trigger
accelerates, left trigger brakes. The same bindings can be changed there; the game starts with
the configured Start control.

Display settings, antialiasing, sound, and saves are available from the menu. Virtua Racing
currently runs on the Model 1 interpreter; native recompilation is not available yet.

CREDITS
Built on MAME's work on the Model 1 (the V60 CPU, TGP, sound and video), Musashi, ymfm, sljit,
SDL, RmlUi and FreeType: see LICENSES.txt.
