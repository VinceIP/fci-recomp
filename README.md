# fci-recomp

A native Windows port of **Far Cry Instincts** (2005, original Xbox) and **Far Cry Instincts: Evolution**
(2006, original Xbox), built by statically recompiling the code from a disc image. Code is translated to C
ahead of time and compiled into a Windows program, which runs alongside a reimplementation of the Xbox kernel,
Direct3D and audio.

Note: this repository contains no original copyrighted game code, data, assets, or executables. An original
game disc is required for the recompilation to run.

DISCORD: https://discord.gg/UhRnapmH2j

### AI Use Disclosure

This project was made using extensive AI assistance and LLM-related tools. A human with a programming background is in the loop throughout development, and I review as much code and documentation as I can. Every effort has been made by me and my human brain to ensure the contents of this repository are of the highest quality, maintainability, and free of AI slop-isms. This project is the result of many long days of building, testing, researching, documenting, and debugging.

Regardless of your stance on AI in software development, it's my hope that this project brings value to fans of this game and to those who care about game preservation. This project will be an ongoing development effort. It is not a "one-shot", and not intended to be a quick and dirty, low-quality effort. Please feel free to raise an issue if you spot any aspects of this project that could be refactored, improved, or de-slopped.

## Status

**Version 0.2.2** - Story mode map editors for Instincts and Evolution are working, but have not yet been thoroughly tested end-to-end. Please continue to report crashes and issues in the Discord or Github. Enhanced visual features powered by a DirectX 12 renderer are implemented. Mouse and keyboard controls have been implemented and are customizable. Enhanced features and options are matched for both separate titles.

## Features

- 2 recomps in 1; both Far Cry Instincts and its follow up sequel Far Cry Instincts: Evolution.
- Single player story mode for both games as they were on the original Xbox.
- Map editors for both games as they were on the original Xbox. Building them is optional and can be skipped if you aren't interested in making maps.
- Mouse and keyboard support. Keybinds and mouse settings can be adjusted by modifying fci.ini.
- Modern visual enhancements via a modified version of [Xbox XBE Recomp](https://github.com/TGP482/Xbox-XBERecomp)
tailored for Far Cry Instincts. Anisotropic filtering, anti-aliasing, screen-space ambient occlusion, scalable
internal rendering resolution, soft shadows, scalable sun shadow resolution, borderless fullscreen, increased 
draw distances, 16:9 widescreen, and more. Display & graphics options can be adjusted by modifying fci.ini.

## Roadmap

Some features are not yet complete or working as intended, but are planned.
- Splitscreen multiplayer
- Online multiplayer
- Share custom maps easily by dropping them into a folder
- A modding framework and map editor enhancements
- Additional rendering and shader enhancements.
- Finish frame rates above 30: vehicles and AI at the game's full rate.

## Requirements

- Windows 10 or 11 64-bit, and a GPU that supports Direct3D 12
- About 10 GB of free space
- An internet connection, or access to Git, Python, Windows SDK, and MSVC
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
3. Wait for it to finish: expect 15-25 minutes depending on your hardware, plus downloads for required
Python and SDK runtimes. You may be prompted to accept terms for Microsoft's compiler if it's not
already on your system. Consider raising an issue if you have any errors during the recompilation/build
process.
4. (Optional) Drag your disc image onto `BuildEditor.cmd` to recompile and build the game's map editor if you intend to make maps, which you should, because it's pretty cool.
5. Start `FarCryInstincts\FarCryInstincts.exe`, or `FarCryEvolution\FarCryEvolution.exe`.
6. Repeat the above steps for each title.

Save data and log files are stored in `%APPDATA%\fci-recomp` by default. This directory can be changed by modifying `fci.ini`. Note that there are separate .ini files for each title. The `build` folder contains an object cache meant to keep future updates faster in some cases. It can be deleted safely, but keep it to speed up future updates.

Developers can run the same steps via Python: `python fci-recomp.py setup <disc>`
(`--help` lists the options).

## Configuration

Directories, display, graphics, keybinds, and other settings are configured by editing `fci.ini`,
found next to the game executable. For more information on what each setting does, read the comments
included in the ini. To change a setting, remove the preceding semicolon `;` from a line and change values
to your desired settings. Save and relaunch the game.

## Known issues

- Crashes may rarely occur during single player mode or elsewhere. Please report any crash and include your crash log to help improve the project.
- Xbox Live and system link don't work yet. Trying to access these features may cause a crash.
- May have some memory management issues, particularly when switching between the map editor mode and back again.
- Frame rates above 30 are experimental (see `display-frame-rate` in fci.ini).
- The window is blurry when Windows display scaling is above 100%.
- Not yet tested: the whole single player mode end to end, multiplayer or splitscreen.

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
