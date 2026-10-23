# Changelog

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
