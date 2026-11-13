# Changelog

## [0.3.0-beta.2] - unreleased

An update to the online multiplayer beta. Draft: edited as the beta goes.

### Added

- The online server can now ask players on an older build to update, and pause online play for maintenance. The match browser shows UPDATE REQUIRED or SERVER PAUSED when this happens.
- FCI Launcher on a beta now offers the next beta as an update. Players on a regular release are never offered a beta.
- Added typing with your keyboard in the map editor's on-screen keyboard (Save Map name and description), in both games.

### Fixed

- Fixed a host dropping off the match list, or being listed twice, when the next map in its rotation loaded.
- Fixed a Steam Deck host missing players' connections.
- Fixed switching between LAN and INTERNET in the multiplayer menu without restarting the game.
- Fixed two copies of the game on one PC showing up as one player online.
- Fixed a blurry game window on displays scaled above 100% (#4).
- FCI Launcher now says when a release needs a newer launcher, instead of saying the release is too old.

## [0.3.0-beta.1] - 2026-11-12

Private beta of online multiplayer.

### Added

- Added experimental online multiplayer for both Instincts and Evolution.
- Added experimental LAN multiplayer for both Instincts and Evolution.
- Added a "Quit Game" main menu option.
- Added support for typing with your keyboard for on-screen input prompts (cheat codes, profile names, etc). This feature for the map editor TBD.
- Added contextual button glyphs for on-screen input labels - changes depending on user keybinds, and whether or not you're using a gamepad or mouse & keyboard.
- Added controller rumble support.
- Aim assist is now off for mouse & keyboard in multiplayer and stays on for gamepads. An option turns it back on for mouse & keyboard.
- Debug logs are now kept by default, even if the game exits successfully. Up to 3 logs are saved at a time before pruning the oldest one. This behavior can be changed in settings.

### Fixed

- Fixed saves being reported as damaged after closing the game while it was saving (#59). Closing the game now waits for a save in progress to finish.
- Fixed Evolution loads hanging for good after installing into a new folder (on Start, or loading a System Link map).

- Fixed the match browser showing only every second match, and a host missing a second player's connection when two arrived together.
- A player behind a relay can now see and join several matches at once (before, only the first match they reached would work).
- A player hosting a match no longer reads the match list or contacts other matches, which saves requests and relay use.

### Known issues

- Players are named after their game profile.
- Instincts online play has had less testing than Evolution.
- Joining by IP address isn't available yet; matches are found through the match list or on the local network.
- Typing in the map editor's on-screen keyboard isn't supported yet.

## [0.2.4] - 2026-11-07

### Added

- Retooled the build process to use a bundled clang/mingw compiler, Python, and recompiler toolkit, eliminating the requirements for heavy Visual Studio and Windows SDK downloads.
- Builds are much faster. Compiling now takes a couple of minutes per game instead of about ten, and recompiling to C takes about half as long.
- Clear cache in the launcher now removes the Python and Microsoft compiler downloaded by earlier versions, freeing about 2 GB.
- Up to 4 controllers are now supported and detected automatically when connected, enabling local split screen multiplayer.
- Added experimental Steam Deck support. Build on a PC, copy the game to the Deck, and run it with Proton 10 or newer. See the README.

### Fixed

- Fixed an issue where some players in split screen multiplayer rendered incorrectly (#16). 3- and 4-player split screen now draw every view properly.
- Fixed a black screen in 3D and a map editor crash when running under Proton.
- Fixed builds failing with older Windows SDKs ("ID3D12InfoQueue1 undeclared", #49), and other errors caused by the Visual Studio, SDK, or Git requirements (#22, #25, #40).

Built against xboxrecomp 193e2995 and Xbox XBE Recomp 65e959d4, with llvm-mingw 20261006 (LLVM 23.1.3).
Supported discs: Far Cry Instincts title 55530008, version 1 (`data/disc.json`); Far Cry Instincts
Evolution title 55530060, version 3 (`data/evolution/disc.json`).

## [0.2.3] - 2026-10-26

### Fixed

- Fixed an issue where building without Visual Studio installed would fail with "the Windows SDK package lacks [...]", after Microsoft changed how it lists the SDK's files.
- Fixed certain build errors not containing enough information.
- The launcher now offers to move itself to `C:\Games\fci-recomp` when its folder is in OneDrive or is otherwise a bad place to build.

## [0.2.2] - 2026-10-26

### Added

- Added the fci-recomp launcher. Allows building, updating, and launching fci-recomp titles, plus changing in-game settings, from one GUI. Editing .ini files by hand or dealing with the old command line BuildGame.cmd is no longer necessary. Building the old way is still supported if preferred.
- Added support for building updates a little faster by saving a build object cache, so future updates with simple bugfixes don't require a painfully slow, full rebuild.

### Fixed

- Visual Studio wasn't found on Windows in other languages, so the build downloaded Microsoft's compiler instead (#40).
- Building into a new folder at the root of a drive failed the permission check (#40).
- The build failed with "Access is denied" when VS Code or an antivirus had the downloaded files open (#40).
- Trying to build with an unsupported disc now gives a more detailed and informative error output.

Built against xboxrecomp 193e2995 and Xbox XBE Recomp 65e959d4. Supported discs: Far Cry Instincts
title 55530008, version 1 (`data/disc.json`); Far Cry Instincts Evolution title 55530060, version 3
(`data/evolution/disc.json`).

## [0.2.1] - 2026-10-23

### Added

- Experimental, unlocked frame rates above 30, for both Instincts and Evolution. Not enabled by default.
- Experimental, toggleable cap to engine updates. Limits game simulation ticks to 30hz. Meant to fix issues caused by higher frame rates.

### Fixed

- Fixed a crash in Evolution loading the third story level.
- Fixed an issue for both titles causing dynamic lights to flicker and be visible through distant geometry.
- Fixed an issue causing the NVIDIA overlay and others to sometimes be invisible/blocked on switching game executables.

Built against xboxrecomp 193e2995 and Xbox XBE Recomp 65e959d4. Supported discs: Far Cry Instincts
title 55530008, version 1 (`data/disc.json`); Far Cry Instincts Evolution title 55530060, version 3
(`data/evolution/disc.json`).

## [0.2.0] - 2026-10-22

### Added

- Support for Far Cry Instincts Evolution (original Xbox, title 55530060, version 3) as a separate game title. Drag its original disc onto BuildGame.cmd to build it. Evolution has its own separate ini, and all the same enhancements as Instincts.

### Fixed

- Fixed an unseen error that handled inputs on controller port 3 rather than port 1.
- Windows error prompts such as "map editor not built" were shown hidden behind the game window. They now show in front, and pause the active game instance.
- Rare crashes in code the recompiler had not fully translated.

Built against xboxrecomp 193e2995 and Xbox XBE Recomp 65e959d4. Supported discs: Far Cry Instincts
title 55530008, version 1 (`data/disc.json`); Far Cry Instincts Evolution title 55530060, version 3
(`data/evolution/disc.json`).

## [0.1.2] - 2026-10-19

### Added

- Implemented shader caching. Removes micro stuttering caused by compiling shaders on a new game instance. Shaders
  are stored in a configurable path in fci.ini. Leaving this path empty disables shader caching.
- Wrote various fixes and edge-case detection for problems running BuildGame.cmd and BuildEditor.cmd. Scripts now
  check for requirements first. Should resolve issues with building the game for some users.

### Fixed

- Building without Git stopped with “does not match its digest” (#25).
- Downloading the compiler failed when the build folder’s path contained a space.
- BuildGame.cmd failed when started from a PowerShell 7 window.
- The game couldn’t start from a folder with accented or non-English characters in its path, including the
  default save folder for such Windows user names.

Built against xboxrecomp 193e2995 and Xbox XBE Recomp 65e959d4. Supported disc: title 55530008,
version 1 (`data/disc.json`).

## [0.1.1] - 2026-10-18

### Added

- Import save games and user maps copied from a real Xbox: Action Replay zips or a copied `UDATA` folder.
  Run `python -B tools/xbox_import.py <zip or folder>...`; it installs them into the save folder so the
  game accepts them. Existing saves are left alone unless `--replace`, which backs them up first, and
  `--dry-run` only reports. Imported user maps appear in Splitscreen and System Link map selection.

### Fixed

- Firing the carbine or the scout rifle crashed the game and the map editor's Jump In Map (#18).
- Every user map showed as "Damaged Map": the game could not list the files inside a map's save folder.

Built against xboxrecomp 193e2995 and Xbox XBE Recomp 65e959d4. Supported disc: title 55530008,
version 1 (`data/disc.json`).

## [0.1.0] - 2026-10-17

First public release.

### Added

- Build from your own disc: drag an ISO or the disc's files onto `BuildGame.cmd`. It checks the disc
  against the supported release, fetches Python and, without Visual Studio, Microsoft's compiler, then
  recompiles and builds the game. About 15 minutes on a 6-core PC.
- The map editor is kept optional as a separate build (`BuildEditor.cmd`), started from the game's Map Editor menu.
- Story Mode playable from the start; the map editor's terrain tool and Jump In Map.
- Direct3D 12 rendering and XAudio2 audio through the host layers from Xbox XBE Recomp.
- Mouse and keyboard with rebindable controls and 1:1 mouse aim on foot.
- Any resolution, the game's own 16:9 mode, supersampling, anisotropic filtering, ambient occlusion,
  edge anti-aliasing, larger and softer shadows, longer draw distances; all optional in `fci.ini`.
- Borderless fullscreen (Alt+Enter) and an option to skip the startup movies.

Built against xboxrecomp 193e2995 and Xbox XBE Recomp 65e959d4. Supported disc: title 55530008,
version 1 (`data/disc.json`).
