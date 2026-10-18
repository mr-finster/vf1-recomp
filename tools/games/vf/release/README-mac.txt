VIRTUA FIGHTER RECOMP (preview)
===============================

An unofficial fan project, not made, approved or supported by SEGA. Run it at your own risk.

Virtua Fighter (Sega Model 1, 1993), statically recompiled for the PC: the arcade game's own
code is translated to native code on your machine when it starts, and the rest of the Model 1
board is reimplemented around it. No game files are included: you need your own MAME "vf" set.


WHAT YOU NEED
- A Mac with macOS 11 (Big Sur) or later, Apple silicon or Intel.
- Your own copy of the game as MAME's "vf" set (vf.zip). That's all it needs.


FIRST LAUNCH
1. Unzip and move "Virtua Fighter Recomp" to Applications (or anywhere).
2. The app isn't signed, so the first time macOS won't open it. Double-click it once, close the
   message, then go to System Settings > Privacy & Security, scroll down and click Open Anyway.
   (On macOS 14 and earlier you can instead right-click the app, choose Open, then Open again.)
3. It asks for your vf.zip. Choose it. It's copied into
   ~/Library/Application Support/Virtua Fighter Recomp/roms, so you only do this once.

Prefer to keep everything in the game's folder (portable)? Put a file named portable.txt next to
the app, in the same folder (an empty one will do). The game then keeps everything in that folder
and nothing in ~/Library/Application Support: settings, saves, screenshots, log.txt, and your
vf.zip, which it asks for once and copies into a "roms" folder there. (An app that hasn't been
moved since it was downloaded runs from a hidden copy that can't see the file: move it with the
Finder first.)

Keep your ROMs in a folder of your own? Add a line to settings.ini (in the data folder above, or
beside the app in portable mode):  roms_folder = ~/ROMs/vf
or start it from Terminal:  open "Virtua Fighter Recomp.app" --args --roms ~/ROMs/vf
(or set M1_ROMS). The game looks there first for vf.zip (and model1io.zip, if you have it) and only reads it; log.txt says which
folder it used, or where it looked when the zip isn't there (it then looks in the usual place).


CONTROLS (change them in the menu: Esc, or View/Select on a pad)
Keyboard, player 1:  arrows move   Z guard   X punch   C kick   1 start
Keyboard, player 2:  I J K L move  B guard   N punch   M kick   2 start
Pads (Xbox layout):  d-pad/stick move   X guard   A punch   B kick   Y guard+punch (throw)
                     RB punch+kick   LB guard+kick   RT all three   Start start
Esc menu   P pause   F3 reset   F5 render scale   F6 picture shape   F9 frame timings
F11 fullscreen

8BitDo pads on a Mac: use Switch (S) mode.


OPTIONS
Display: render scale (up to 6x), antialiasing, picture shape (fits the window: widescreen and
ultrawide), background filter, fullscreen. Sound: volume, music, effects and voices.
Game: play style (PC: free play, no boot screen / arcade: coins and credits), reset.
Controls: each player's pad, keys and buttons.
Cheats: a secret character in Akira's spot (Siba, Jeff, Takeru, the Koskol brothers or Dural:
cut from the game, unfinished), infinite energy, one-hit KO, no time limit, a tiny or huge ring.
3D renderer: Auto (the default) draws the 3D on the graphics card, the same picture as the CPU,
unless it is clearly slower on your machine; GPU or CPU to choose yourself. If the GPU ever
draws differently or fails, the game uses the CPU for that run (log.txt says so).


YOUR FILES
Settings, saves (rankings, the operator settings) and your ROMs are in
~/Library/Application Support/Virtua Fighter Recomp (or beside the app, with a portable.txt there). Put replacement stage skies in its
"backgrounds" folder (stage_beach_clouds.png and so on) to have them used instead of the originals.


CREDITS
Built on MAME's work on the Model 1 (the V60 CPU, TGP, sound and video), Musashi, ymfm, sljit,
SDL, RmlUi and FreeType: see LICENSES.txt. The recompile was done with Claude Opus 5.5.
