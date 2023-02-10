# Changelog
## Version 0.5.2

- Updated to DevkitARM r46 for better DSi support.
- Added an installable CIA file to run as DSiWare.
- Certain 3DS-side launchers such as TWLoader now work again.
- Added "touch" as a configurable button.
  - When running as a CIA on the New 3DS, the "touch" input gets spammed for no reason.
    It can now be disabled as a workaround.

## Version 0.5.1

- Savegame management has been modified to prevent corruption as reported by some.
- Autosaving is laggier due to the above. Consider turning it off.
- The configuration file has been moved from "gameyob.ini" to "gameyobds.ini" to prevent
  conflicts with GameYob 3DS.
- A "single-screen mode" has been added.

## Version 0.5

- Added Gameboy Printer emulation
- Added GBS playback feature
- Added "scale" button
- Semi-overhauled menu: now colorized, and does not interrupt emulation.
- Improved Super Gameboy emulation
- Selectable custom borders
- Reduced autosaving lag (results may vary between flashcards and SD cards)
- Implemented the halt bug (fixes The Smurfs)
- Wram register has unused bits set (fixes Metal Gear Solid)
- More fixes to emulation accuracy, sound, etc
- Stability fixes (most notably for sleep mode), many other minor bugfixes

## Version 0.4.1

- Fixed "save" button (which was bugged) and reduced autosaving lag

## Version 0.4

- New Icon by Corbin Davenport
- Implemented Super Gameboy support
- Implemented scaling (for those who don't like borders)
- Implemented custom borders (for those who don't like scaling)
- Implemented autosaving (causes lag in some games; off by default)
- Added "Detect GBA" option to access the advance shop in the Oracle games
- Added Fast Forward toggle button
- Added "Sound Fix" option for very accurate sound timing; it may use more power.
- Added Rumble Pak Support for Ez-3in1 and Warioware Twisted carts (thanks to windwakr)
- Added support for Gameshark and Game Genie codes (thanks to LemonBoy)
- Support for Rockman 8, HuC1/HuC3 mappers and Robopon games (thanks to LemonBoy)
- Second screen's backlight is disabled when not needed to save power
- (partial) Stereo sound emulation
- More improvements to emulation & sound accuracy

## Version 0.3

- Surpasses Lameboy in speed by about 10 FPS, thanks to optimizations by Nebuleon and myself
- Added fast forward mode (courtesy of Nebuleon)
- Added support for many exotic graphical effects & fixed screen-change graphical bugs
- Added MBC2 support (for Kid Icarus, Final Fantasy Adventure)
- Added save states and suspending (temporary states)
- Added option to save settings to gameyob.ini
- Added HIGHLY EXPERIMENTAL NiFi link cable emulation (works with Tetris, Dr. Mario, and Pokemon if you're lucky)
- Fixed various emulation bugs, increasing compatibility
- Fixed many causes of crashes (DSi mode works now)
- Soft resetting works (L+R+Start+Select)
- 100% remappable controls
- Sound bugfixes

## Version 0.2

- Fixed some sound emulation bugs
- Fixed sleep-mode crashes
- Fixed window behaviour when modified mid-frame (fixes text in Resident Evil Gaiden)
- Fixed interrupt behaviour (fixes boss intros in Megaman V & passes blargg's cpu test #2)
- Fixed crashes in some games without sram (Avenging Spirit)
- Set default # of ram banks to 4 (workaround for Japanese Crystal)
- Added GBC Bios support
- Added "B/Y" control option
- Added a little clock
- Capitalized rom extensions are detected properly

## Version 0.1.2

- Fixed Z flag for sla (hl) and rl (hl) opcodes

## Version 0.1.1

- Removed limit on number of files per directory
