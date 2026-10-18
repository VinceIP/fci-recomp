# Changelog

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
