VIRTUA FIGHTER RECOMP (preview)
===============================

An unofficial fan project, not made, approved or supported by SEGA. Run it at your own risk.

Virtua Fighter (Sega Model 1, 1993), statically recompiled for the PC: the arcade game's own
code is translated to native code on your machine when it starts, and the rest of the Model 1
board is reimplemented around it. No game files are included: you need your own MAME "vf" set.


WHAT YOU NEED
- Windows 10 or 11, 64-bit.
- Your own copy of the game as MAME's "vf" set (vf.zip). That's all it needs.


FIRST LAUNCH
1. Unzip the VirtuaFighter folder anywhere (not inside Program Files).
2. Run VirtuaFighter.exe. It isn't signed, so Windows SmartScreen may warn the first time:
   click "More info", then "Run anyway".
3. It asks for your vf.zip. Choose it. It's copied into
   %APPDATA%\Virtua Fighter Recomp\roms, so you only do this once.

Prefer to keep everything in the game's folder (portable)? Put a file named portable.txt next to
VirtuaFighter.exe (an empty one will do: right-click > New > Text Document, and name it
portable). The game then keeps everything in its own folder and nothing in %APPDATA%: settings,
saves, screenshots, log.txt, and your vf.zip, which it asks for once and copies into its "roms"
folder. (A "roms" folder next to VirtuaFighter.exe with vf.zip in it keeps the rest there too,
but a copy of the zips also goes to %APPDATA%, so a new version unzipped elsewhere finds them.)

Keep your ROMs in a folder of your own? Add a line to settings.ini (in the data folder, or next
to VirtuaFighter.exe in portable mode):  roms_folder = D:\ROMs\vf
or give it to the program: a shortcut whose Target ends in  --roms "D:\ROMs\vf"  (or set the
environment variable M1_ROMS). The command line wins, then M1_ROMS, then settings.ini. The game looks
there first for vf.zip (and model1io.zip, if you have it) and only reads it; log.txt says which folder it used, or where it
looked when the zip isn't there (it then looks in the usual places, and a zip you then choose is
copied into your ROM folder).


CONTROLS (change them in the menu: Esc, or View/Select on a pad)
Keyboard, player 1:  arrows move   Z guard   X punch   C kick   1 start
Keyboard, player 2:  I J K L move  B guard   N punch   M kick   2 start
Pads (Xbox layout):  d-pad/stick move   X guard   A punch   B kick   Y guard+punch (throw)
                     RB punch+kick   LB guard+kick   RT all three   Start start
Esc menu   P pause   F3 reset   F5 render scale   F6 picture shape   F9 frame timings
F11 fullscreen



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
%APPDATA%\Virtua Fighter Recomp (or next to the program, with a portable.txt or a "roms" folder
there). Put replacement stage skies in its
"backgrounds" folder (stage_beach_clouds.png and so on) to have them used instead of the originals.


CREDITS
Built on MAME's work on the Model 1 (the V60 CPU, TGP, sound and video), Musashi, ymfm, sljit,
SDL, RmlUi and FreeType: see LICENSES.txt. The recompile was done with Claude Opus 5.5.
