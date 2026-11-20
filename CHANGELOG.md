# Changelog

## [0.3.0] - unreleased

### Added

- Added experimental online multiplayer for both Instincts and Evolution. Matches are found through the match list in Multiplayer.
- Added experimental LAN multiplayer for both Instincts and Evolution.
- The match browser says when the online server needs a newer build (UPDATE REQUIRED) or is paused for maintenance (SERVER PAUSED).
- Multiplayer always runs the game engine at the original 30 updates a second, whatever the frame rate settings, so physics and AI stay in step between players.
- Added quick save (F5) and quick load (F9) in Story Mode, in both games. A quick save is the level's checkpoint: Story Mode's Resume continues from it, and the game's next checkpoint replaces it. The keys can be changed in fci.ini.
- Added an in-game settings screen: Controller Options holds the control settings, and a new GRAPHICS button (on the profile screen and in the pause menu's Options) opens the display and graphics settings.
- Added a field of view setting (`display-fov`).
- Added 16:10, 21:9, 32:9 and auto aspect ratios (`display-aspect`). The 3D view uses the true aspect; the HUD is repositioned at 16:10 and kept in a centred 16:9 box at wider ratios.
- Added experimental volumetric fog, with light shafts, a ground layer and haze (`gfx-fog = 3`, off by default). F8 switches between it and the original fog while playing. It is thicker than the original fog for now.
- Added a bloom setting (`gfx-bloom`).
- Added V-sync and frame rate display settings (`display-vsync`, `display-show-fps`). F11 now also toggles fullscreen.
- Added a "Quit Game" main menu option.
- Added typing with your keyboard in on-screen keyboards (cheat codes, profile names, and the map editor's Save Map name and description).
- Added button prompts that follow your key bindings and whether you're playing with a gamepad or mouse and keyboard.
- Added controller vibration.
- Aim assist is now off for mouse and keyboard, and stays on for gamepads. An option turns it back on for mouse and keyboard.
- Debug logs are now kept by default, even when the game exits normally. The last 3 sessions are kept (`log-limit`), and a fixed `log =` path keeps earlier sessions too.
- FCI Launcher's Settings tab includes the new settings.

### Fixed

- Fixed profiles reported as "damaged and cannot be used" after starting a level from Load a Map following a checkpoint, in both games. Profiles already affected are repaired at startup; the removed checkpoint is kept in the `save-backups` folder.
- Fixed saves being reported as damaged after closing the game while it was saving (#59). Closing the game now waits for a save in progress to finish.
- Fixed a crash when renaming a profile, which also broke its saves.
- Fixed bump-mapped surfaces drawn far too dark (the Training tunnel's walls, the player's hands). They now show their lighting and detail.
- Fixed models drawn inside out or with missing faces.
- Fixed shadow acne and striping, and shadow edges crawling, on the sun's shadows (#50).
- Fixed geometry vanishing and coming back as the player moved, such as the wooden beams in the Rain Forest mines (#31).
- Fixed shadows flickering and jumping, and one-frame black flashes, above 30 fps.
- Fixed ambient occlusion drawing over water, grass and distant fog.
- Fixed objects disappearing at the edges of a wide field of view in Evolution.
- Fixed Evolution loads hanging for good after installing into a new folder (on Start, or loading a System Link map).
- Fixed a blurry game window on displays scaled above 100% (#4).

### Known issues

- At a wide field of view, the screen edges can show stale image strips indoors and a dark band in some places.
- Players are named after their game profile.
- Instincts online play has had less testing than Evolution.
- Joining by IP address isn't available yet; matches are found through the match list or on the local network.

Built against xboxrecomp 193e2995 and Xbox XBE Recomp 65e959d4, with llvm-mingw 20261006 (LLVM 23.1.3).
Supported discs: Far Cry Instincts title 55530008, version 1 (`data/disc.json`); Far Cry Instincts
Evolution title 55530060, version 3 (`data/evolution/disc.json`).

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
