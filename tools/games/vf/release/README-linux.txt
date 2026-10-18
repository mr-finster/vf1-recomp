VIRTUA FIGHTER RECOMP for Linux and the Steam Deck (preview)
=============================================================

An unofficial fan project, not made, approved or supported by SEGA. Run it at your own risk.
No game files are included: you need your own MAME "vf" set (vf.zip).

INSTALL
1. Copy the download to the Deck (e.g. Downloads) and unpack it there: in Desktop Mode, right-click
   it in the file manager > Extract > Extract archive here. (Unpacking on Windows, or copying the
   folder through a USB stick, can lose the files' permissions, and then nothing launches.)
2. Put your vf.zip in the VirtuaFighter/roms folder.
3. Desktop Mode: Steam > Games > Add a Non-Steam Game to My Library > Browse, set the file type
   filter to "All files", pick the vf file in the VirtuaFighter folder, Add Selected Programs.
   Then in its Properties: Start In is the VirtuaFighter folder (e.g. /home/deck/Downloads/
   VirtuaFighter/), and under Compatibility, "Force the use of a specific Steam Play
   compatibility tool" is off (it's a native Linux game: Proton stops it launching).
4. Back in Game Mode, launch it from your Library with the default Gamepad layout.
On another x86-64 Linux: unpack it anywhere, put vf.zip in VirtuaFighter/roms and run VirtuaFighter.sh.

YOUR OWN ROM FOLDER (Batocera, a ROMs folder an update won't touch)
To keep vf.zip somewhere else, tell the game where it is, in any of these ways (the first one given wins):
  - on the command line: ./vf --roms /userdata/roms/ports/vf   (or --roms=/userdata/roms/ports/vf);
    in Steam: the game's Properties > Launch Options: --roms "/home/deck/ROMs/vf"
  - the environment variable M1_ROMS=/userdata/roms/ports/vf
  - a line in settings.ini (in this folder):  roms_folder = /userdata/roms/ports/vf
    (~ is your home folder; a relative path is from settings.ini's folder)
For example, a Batocera port launcher, /userdata/roms/ports/VirtuaFighter.sh:
  #!/bin/bash
  exec /userdata/system/VirtuaFighter/VirtuaFighter.sh --roms /userdata/roms/ports/vf
The game only reads that folder (log.txt says which folder it used). If vf.zip isn't there, log.txt says
where it looked and the game uses the roms folder here as before; a set you then choose with the
file picker is copied into your ROM folder, so the next start finds it there.

CONTROLS
  D-pad / left stick   move            X  guard     A  punch     B  kick
  Y   guard + punch (throws)           RB punch + kick           LB guard + kick
  RT  guard + punch + kick             Start  start
  View (tap)  the menu: display, sound, controls and quit
  View + LB / RB   render scale / picture shape

Display > 3D renderer: Auto (the default) draws the 3D on the GPU (Vulkan), the same picture as
the CPU, unless it is clearly slower; GPU or CPU to choose yourself.

Settings, rankings and saves are kept in this folder; a copy of your zips also goes to
~/.local/share/Virtua Fighter Recomp, so a new version unpacked elsewhere finds them. To keep
everything in this folder and nothing outside it, put a file named portable.txt here. See LICENSES.txt for the software it uses.
