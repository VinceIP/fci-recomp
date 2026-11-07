# Third-party components

This project's own code is MIT licensed ([LICENSE](LICENSE)). It includes or patches the
following components, each under its own license.

## In this repository

| Component | Where | License | Copyright |
|---|---|---|---|
| [Xbox XBE Recomp](https://github.com/TGP482/Xbox-XBERecomp) at 65e959d4: Direct3D 8 and DirectSound layers, Direct3D 12 renderer | `src/xbr` (vendored, with changes marked `FCI`) | MIT, [src/xbr/LICENSE](src/xbr/LICENSE) | 2026 TGP482 |
| Patches to [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) | `patches/` | MIT, except the parts that change xemu's LGPL files, which are LGPL-2.1-or-later ([patches/README.md](patches/README.md)) | this project |

`tools/sigmatch.py` reimplements Xbox XBE Recomp's signature matcher (`tools/symbols/main.cpp`, MIT).

## In the launcher

`launcher/` builds into `FCI Launcher.exe`, which contains these. It contains no code from the game and
none of the LGPL code below.

| Component | Where | License |
|---|---|---|
| [Archivo](https://github.com/Omnibus-Type/Archivo), [Saira](https://github.com/Omnibus-Type/Saira) ExtraCondensed and [IBM Plex Mono](https://github.com/IBM/plex) fonts | `launcher/ui/fonts` | SIL Open Font License 1.1 (the `OFL-*.txt` files there) |
| [Tauri](https://tauri.app) and the other Rust crates in `launcher/src-tauri/Cargo.toml` | built into the executable | MIT or Apache-2.0 (each crate's own) |

## Included in the release zip

The release zip carries these beside this project's files, so a build downloads nothing. A git checkout
fetches them instead (`tools/toolchain.py`, `tools/deps.py`, `tools/bootstrap.ps1`).

| Component | Where | Used for | License |
|---|---|---|---|
| [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 20261006 (LLVM 23.1.3 clang, lld, the mingw-w64 headers and libraries), trimmed to what the build uses | `toolchain/` | compiling | LLVM: Apache-2.0 with LLVM exception; mingw-w64: ZPL, public domain and other permissive licenses; see `LICENSE.TXT` there |
| [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) at 193e2995 | `deps/xboxrecomp` | the lifter and the Xbox runtime | MIT, 2026 sp00nz |
| the [xemu](https://xemu.app) files in xboxrecomp (`src/apu`, `src/nv2a/nv2a_regs.h`) | `deps/xboxrecomp` | the APU model | LGPL-2.1-or-later, see xboxrecomp's `NOTICE` |
| [Xbox XBE Recomp](https://github.com/TGP482/Xbox-XBERecomp) at 65e959d4 | `deps/xbe-recomp` | its XDK 5849 signature list | MIT, 2026 TGP482 |
| [Python](https://www.python.org) 3.13.16 | `python/` | running the build tools | PSF License (`python/LICENSE.txt`) |
| [Capstone](https://www.capstone-engine.org) 5.0.7 | `python/Lib/site-packages` | disassembly in the lifter and the signature matcher | BSD-3-Clause |

## LGPL and the executables you build

The executable the build produces links the LGPL-licensed APU code statically. It is built on your own
machine from source you have, and this project distributes no game executables, so you can always rebuild it
with a modified version of that code. Anyone who distributes a built executable must meet the LGPL's
terms for it themselves, and must not distribute it at all, since it contains code translated from the
game.
