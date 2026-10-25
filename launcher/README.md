# fci-recomp launcher

One program that builds, updates and starts the games, for players who would rather not use
`BuildGame.cmd`. It is a Tauri 2 app: Rust in `src-tauri/`, the page in `ui/` (plain HTML, CSS and
JavaScript, no build step).

The launcher doesn't build anything itself. It runs `fci-recomp.py` from a release with `--events`
(see `tools/events.py`) and shows those events as a progress bar and two lines of status. So
`BuildGame.cmd`, the launcher and the command line all build the same way.

## What it does

- **Finds the games** in the folders beside it (`FarCryInstincts\`, `FarCryEvolution\`), with the version
  from each game's `build.json`. A folder `BuildGame.cmd` built in works the same.
- **Checks a disc as soon as it is chosen** (picked, or dropped on the window). It reads the disc's
  default.xbe and says what's wrong with an unsupported one (another region or version, a patched
  default.xbe, another game). The build still checks every file before it copies anything.
- **Builds** the game, and optionally the map editor, with `fci-recomp.py setup --events`. A map
  editor added later is built with `editor`.
- **Updates.** When GitHub has a newer release, it downloads the release zip, checks it against the
  SHA-256 GitHub lists for it, unpacks it into `source\<version>\`, and runs `fci-recomp.py update`
  for each game built here. The input digests (`lift.current`, `build.current`) and the object cache
  decide what is rebuilt, so the disc isn't needed.
- **Starts** the game or its map editor.
- **Settings:** the options of each game's `fci.ini`, read from the release's `src/fci.ini`. They can
  be kept the same for both games, except the folders each game keeps for itself.
- **Logs:** every build writes `logs\<date>-<game>-<kind>.log`. A failed build gets a report, without
  the user's name, to copy or to open as a pre-filled GitHub issue.

Microsoft's compiler license is asked about in a dialog: `fci-recomp.py` reports it as an event and
stops with exit code 3, and the launcher starts the build again with `--accept-msvc-license`.

## Layout of the launcher's folder

```
FCI Launcher.exe
launcher.json         the discs chosen, settings sync
source\<version>\     releases of fci-recomp (the two newest are kept)
build\                the work folder all versions share: Python, compiler, toolkit, lift, build
FarCryInstincts\  FarCryEvolution\
logs\
```

A release zip (`fci-recomp-<version>.zip`) placed beside the launcher is used as if it had been
downloaded, when it is newer than what's there. That works offline, and for testing a build before
it is released.

## Building

Needs Rust (stable) and, on Windows, the WebView2 runtime (part of Windows 10 and 11).

```
cd launcher/src-tauri
cargo test
cargo build --release        # target/release/fci-launcher.exe
```

The page and its files are built into the executable. Release it as `FCI-Launcher-<version>.exe`
(the version in `Cargo.toml`): the launcher looks for an asset with that name in the latest release
to update itself.

From Linux, `cargo xwin build --release --target x86_64-pc-windows-msvc` cross-compiles it (with
[cargo-xwin](https://github.com/rust-cross/cargo-xwin)).

For working on the page without a disc or Windows, these variables help:

- `FCI_LAUNCHER_ROOT`: the folder to use instead of the executable's.
- `FCI_PYTHON`: the Python to run `fci-recomp.py` with, instead of `build\python`.

## Artwork

The banners are `ui/art/instincts.jpg` and `ui/art/evolution.jpg`, the games' covers, 2:3 portrait
(600x900 or larger). Until a game is built, its banner is shown greyed out. For a monochrome version
made by hand, set `mono` for the title in `ART` at the top of `ui/app.js`.

The fonts in `ui/fonts` are Archivo (text), Saira ExtraCondensed (names and headings) and IBM Plex
Mono (versions, paths, numbers), under the SIL Open Font License (the `OFL-*.txt` files beside them).
