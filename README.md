# fci-recomp

A native Windows port of **Far Cry Instincts** (2005, original Xbox) and **Far Cry Instincts: Evolution**
(2006, original Xbox), built by statically recompiling the code from a disc image. Code is translated to C
ahead of time and compiled into a Windows program, which runs alongside a reimplementation of the Xbox kernel,
Direct3D and audio.

Note: this repository contains no original copyrighted game code, data, assets, or executables. An original
game disc is required for the recompilation to run.

DISCORD: https://discord.gg/UhRnapmH2j

### AI Use Disclosure

This project was made using extensive AI assistance and LLM-related tools. A human with a programming background is in the loop throughout development, and I review as much code and documentation as I can.

## Status

**Version 0.3.0.beta** - Active beta test pre-release version for online multiplayer support. Old, non-functioning Xbox Live
menus have been replaced with an online server browser accessible from within the game. Hosting and joining games with other real players works. Sharing and playing on custom maps between peers works as it did in the original game. Hosting a dedicated server with rotating maps also works as it did in the original game. Various QOL improvements have been added, such as context-sensitive controller/input icons and controller rumble.

**Version 0.2.4** - Current main, stable release. Story mode and map editors for Instincts and Evolution are working. Frame rates can be unlocked up to your display's refresh rate. Enhanced visual features powered by a DirectX 12 renderer are implemented. Mouse and keyboard controls drive the game's camera itself as opposed to manipulate the controller analog stick. Keybinds are customizable. Enhanced features and options are matched for both separate titles. Some visual glitches remain. Please continue to report crashes and issues in the Discord or Github!

## Features

- 2 recomps in 1; both Far Cry Instincts and its follow-up sequel Far Cry Instincts: Evolution.
- Single player story mode for both games as they were on the original Xbox.
- Map editors for both games as they were on the original Xbox. Building them is optional and can be skipped if you aren't interested in making maps. Enhancements to the map editors are planned!
- Mouse and keyboard support. Keybinds and mouse settings can be adjusted by modifying fci.ini. The mouse manipulates the game camera directly, rather than simply driving a digital analog stick.
- Modern visual enhancements via a modified version of [Xbox XBE Recomp](https://github.com/TGP482/Xbox-XBERecomp)
tailored for Far Cry Instincts and Evolution. Anisotropic filtering, anti-aliasing, screen-space ambient occlusion, scalable
internal rendering resolution, soft shadows, scalable sun shadow resolution, borderless fullscreen, increased 
draw distances, 16:9 widescreen, and more. Display & graphics options can be adjusted by modifying fci.ini.

## Roadmap

See our `#roadmap` on the [Discord channel!](https://discord.gg/MupYxBrrQT)

## Requirements

- Windows 10 or 11 64-bit, and a GPU that supports Direct3D 12
- About 10 GB of free space
- Nothing else to install, as of 0.2.4 - the release includes the compiler and the other build tools
- A dump of at least one of the following game discs, as an ISO or extracted files:
  - Far Cry Instincts (original Xbox, title ID `55530008`, version 1)
  - Far Cry Instincts: Evolution (original Xbox, title ID `55530060`, version 3)

## Setup

### With the launcher

1. Download the zip of the [latest release](../../releases/latest) and extract it.
2. Start `FCI Launcher.exe`. Windows may warn that it's from an unknown publisher, since the launcher isn't signed. Click "More info", then "Run anyway".
3. Choose your disc image (or drop it on the window), then Build. Tick the map editor box to build it too.
4. Launch your game.

### With BuildGame.cmd

1. Download the zip of the [latest release](../../releases/latest) and extract.
2. Drag your disc image (or the folder with the disc's files) onto `BuildGame.cmd`.
3. Wait for it to finish: expect 5-20 minutes depending on your hardware. Nothing is downloaded or
installed. Consider raising an issue if you have any errors during the recompilation/build process.
4. (Optional) Drag your disc image onto `BuildEditor.cmd` to recompile and build the game's map editor if you intend to make maps, which you should, because it's pretty cool.
5. Start `FarCryInstincts\FarCryInstincts.exe`, or `FarCryEvolution\FarCryEvolution.exe`.
6. Repeat the above steps for each title.

Save data and log files are stored in `%APPDATA%\fci-recomp` by default. This directory can be changed by modifying `fci.ini`. Note that there are separate .ini files for each title. The `build` folder contains an object cache meant to keep future updates faster in some cases. It can be deleted safely, but keep it to speed up future updates.

Developers can run the same steps via Python: `python fci-recomp.py setup <disc>`
(`--help` lists the options).

A git checkout doesn't include the compiler and toolkit; the first build downloads them once
(`python -B tools/toolchain.py fetch` does it ahead of time).

## Steam Deck (experimental)

Both Instincts and Evolution run on the Steam deck via Proton, but this is experimental. Expect some issues here and there.

1. Build the game (and the map editor, if you want it) on a Windows PC as above.
2. Copy the whole game folder (`FarCryInstincts` or `FarCryEvolution`, with its `game` folder inside) to the Deck.
3. In Desktop Mode, add `FarCryInstincts.exe` or `FarCryEvolution.exe` to Steam as a non-Steam game.
4. In the game's Properties > Compatibility, force Proton 10, Proton stable, or newer.
5. Launch it from Steam.

A full Linux port is being planned at some point in the future, and the need for Proton will go away.

The launcher (`FCI Launcher.exe`) doesn't run on the Deck yet, but it will be ported to work on Linux soon. You'll need to manually adjust your fci.ini to customize settings on the Steam Deck for now.
Remember to remove semi-colons from the setting you want to adjust in the file.

## Configuration

Adjust game settings under the `Settings` tab in `FCI Launcher`.

Alternatively - directories, display, graphics, keybinds, and other settings are configured by editing `fci.ini`,
found next to the game executable. For more information on what each setting does, read the comments
included in the ini. To change a setting, you must remove the preceding semicolon `;` from a line and change values
to your desired settings. Save and relaunch the game.

## Known issues

- Crashes may rarely occur during single player mode or elsewhere. Please report any crash and include your crash log to help improve the project.
- Occasional freezes on Steam Deck/Linux under Proton, requiring a force quit.
- Frame rates above 30 are experimental (see `display-frame-rate` in fci.ini). AI and physics may break or cause a game softlock, unless the engine update cap setting is enabled.
- Some areas in single player appear too dark, don't light up correctly, have flickering shadows, or issues with vertices disappearing depending on where the player is standing.

Logs of crashed sessions are kept in `%APPDATA%\fci-recomp\logs`. Consider raising an issue and attaching a crash
log if your game crashes, or report any other technical issues.

## Legal

This is an unofficial fan project, not affiliated with or endorsed by Ubisoft, Crytek or Microsoft. Far
Cry is a trademark of Ubisoft Entertainment; Xbox is a trademark of Microsoft. All trademarks belong to
their owners.

The repository contains no code, data or assets from the game, and no Microsoft software. You need a
legally obtained copy of the original game. Requests for game copies, or links to them, will be removed.

## License

This project's code is released under the [MIT license](LICENSE). Third-party components keep their own
licenses: see [THIRD_PARTY.md](THIRD_PARTY.md). That includes Xbox XBE Recomp (MIT), xboxrecomp (MIT,
with LGPL-2.1 parts from [xemu](https://xemu.app)), and our patches to those LGPL files, which are
LGPL-2.1 as well.

## Thanks

[sp00nz](https://github.com/sp00nznet) for xboxrecomp, [TGP482](https://github.com/TGP482) for Xbox XBE
Recomp, and the [xemu](https://xemu.app) developers, whose hardware research this builds on.
