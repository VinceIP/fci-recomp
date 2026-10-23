# Host Direct3D and DirectSound (xbr)

The Direct3D 8 layer, Direct3D 12 renderer and DirectSound layer of **Xbox XBE Recomp**
([TGP482/Xbox-XBERecomp](https://github.com/TGP482/Xbox-XBERecomp), revision
65e959d4a5fd0c171dc7b17f3f9b44bab00928e6, MIT, see `LICENSE`), vendored with this project's changes.
They replace the game's lifted `D3DDevice_*` and DirectSound functions (XDK 5849) with host code that
draws through Direct3D 12 and plays through XAudio2.

## Layout

| Path | Origin |
|---|---|
| `gpu/`, `xdk/`, `audio/`, `xbe/xbe.h`, `core/`, `cpu/x86_context.h` | upstream `runtime/`, same relative paths |
| `cpu/x86_ops.h`, `kernel/`, `app/` | this project: the parts of the upstream headers the vendored code uses, implemented on this runtime |
| `fci_xbr_glue.cpp` | this project: guest calls, memory, kernel events, host threads, the window, logging, options |
| `fci_enhance.cpp`, `fci_gfx.h` | this project: rendering enhancements (a `gpu::Extension`) and the graphics options |

`image_symbols.h`, the addresses `tools/sigmatch.py` finds in the game's executable, is generated during
the build and never committed. Hand-verified addresses the signatures cannot find are in
`data/default.pins.json` and `data/ige.pins.json`.

## Wiring

- The folder is compiled as C++20. Each `X86_FUNC(name)` becomes `xbr_sub_<va>`; the symbol tables of
  `xbr_d3d8.obj` and `xbr_dsound.obj` list them in `xbr-replace.json` (DirectSound's under `audio`).
- `tools/compile.py` turns each listed `sub_<va>` in the generated code into
  `fci_xbr_enabled() ? xbr_sub_X() : xbr_orig_sub_X_c()`, keeping the lifted body under the second name.
  DirectSound's functions switch on `fci_xbr_audio_enabled()`. Wrapped functions the image lacks get
  generated stubs that stop if reached.
- Host code runs on the lifted thread's registers, copied into and out of an `X86Context` around each
  call.

## Changes to the vendored files

Every change is marked `FCI` in the source.

- `core/log.h`: no `format` attribute under MSVC.
- `xdk/hle.h`: `GUEST_ORIG` names the renamed lifted body.
- `gpu/d3d12.cpp`: Dear ImGui compiled out; render scale per axis; a 32 bit float depth buffer;
  visibility tests as occlusion queries; `Extension::Present`, a post-processed picture shown in place of
  the presented target; box filtering of a picture larger than the window; a lost device stops (99);
  diagnostics (`XBE_PROBE`, `XBE_CAPTURE_VS`, the debug layer's messages to the log with `XBE_D3D12_DEBUG`);
  presenting without waiting in test runs; frame replay (`RECOMP_FCI_XBR_REPLAY`: a frame drawn in one
  command list is kept and executed again for display refreshes between the title's presents, each
  resource it changes first returned to the state it began in; its draws are matched with the frame
  before's, by an exact key and a loose one, the nearest constants among draws sharing a key; each replay
  blends the registers the vertex programs' positions depend on, from the frame before to the kept frame:
  registers one value holds for every draw (the camera) for all draws, the others for exact matches and
  loose keys drawn once, when they moved by at most half; `RECOMP_FCI_XBR_REPLAY_DUMP` saves what is shown).
  A presenter thread shows one picture each refresh: a kept frame as the render thread executed it, then
  replays, each only once the GPU has finished the last (the title's frames come first); the render thread
  hands kept frames over and presents the others itself. Submissions and the kept frame share one lock,
  the swap chain another.
- `gpu/shaders.cpp`, `gpu/shaders.h`: W-buffered depth written from w in the pixel shader, with polygon
  offset applied there; bump map du dv converted to signed values before filtering; `_umul128` instead
  of `__int128`; the `XBE_PS_SHOW` diagnostic.
- `gpu/backend.h`, `gpu/render_thread.cpp`: visibility tests in order with the draws; the ring of
  command data reused safely after a lap; the game thread's wait for the render thread gives up the
  guest CPU.
- `gpu/ext.h`: render scale per axis for new targets; the present hook.
- `xdk/d3d8.cpp`: surfaces from the game's own XMemAlloc; references taken on a surface's parent;
  RunPushBuffer replayed as indexed draws; the PVIDEO overlay composed into the frame; visibility tests;
  freed contiguous memory handed back once the renderer is past it; waits that give up the guest CPU;
  the presentation interval and the widescreen flag passed to the present; diagnostics.
- `xdk/dsound.cpp`: buffer memory from the system heap.
- `audio/xaudio2.cpp`: after a critical error (the output device removed or changed) the output is
  rebuilt on the new default device.

## DirectSound coverage

The game calls 50 DirectSound functions. Forty-nine are implemented by `xdk/dsound.cpp`, directly or
through thin wrappers; the 50th, 0057D74E, a media decoder that only produces samples for a stream,
stays lifted. Not reproduced: the DSP effects (I3DL2 reverb and the effects image's other effects; the
image is read so the game finds its effect indices), voice filters, envelopes and HRTF. 3D sound is
distance attenuation with rolloff curves and stereo panning.
