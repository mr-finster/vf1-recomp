VIRTUA FIGHTER RECOMP for the TrimUI Smart Pro / Brick Pro (community build)
===========================================================================

An unofficial fan project, not made, approved or supported by SEGA. Run it at your own risk.
No game files are included: you need your own MAME "vf" set (vf.zip, with 315-5724.bin, the TGP's program)
and, if you have it, model1io.zip.

This build is checked on the consoles by the people who made it, not by the project: tell them (or open an
issue) if something's wrong, with launch.log and log.txt from this folder.

INSTALL
Unpack the download on a computer and copy the whole VirtuaFighter folder to the console's SD card:
  - the stock firmware:  into Apps/  (it appears among the apps)
  - Mosaik:              into Arcade/Linux/  (among the Linux games)
  - another custom firmware: where it keeps Linux "ports" that start a launch.sh
Then put your vf.zip (and model1io.zip) in VirtuaFighter/roms/.
(Copy the folder from macOS or Linux, or from Windows with a tool that keeps the files' permissions; if
nothing starts, launch.log says why.)

PLAY (the console's own buttons)
  D-pad    move              Y  guard               B  punch              A  kick
  X        guard + punch     R  punch + kick        L  guard + kick       R2 all three
  Start    start             Select  the menu (in menus A chooses, B goes back; Quit is there)
Change them in the menu, Controls.

ON THIS CONSOLE
  - One profile, the lightest: the arcade's 1x (496 x 384, each line doubled on a Brick Pro), no
    antialiasing, the 3D on the CPU, no smooth motion, vsync. The Display > Quality and Screen pages
    aren't in the menu. The game runs at the arcade's 57.5 frames a second, with about 40 pictures a
    second on a Brick Pro.
  - The sound board runs on a core of its own.
  - No Online for now: a rollback runs frames again, and these CPUs have little room to spare.
  - launch.sh sets the CPU to 2000 MHz while the game runs, and back after.
  - The frame rate on screen: menu, Display > Picture > Show FPS.

SWITCHES (an empty file in this folder with that name)
  scale2       the 3D at 2x (992 x 768): sharper, but about half the pictures a second
  soundinline  the sound board in the game's frames, as on a PC (fewer pictures a second: to compare)
  profile      timings in log.txt every two seconds
  bench        one start measures the game as it is, with soundinline and with scale2 on a scripted fight
               (about 5 minutes, don't touch the pad): the results in bench.txt
