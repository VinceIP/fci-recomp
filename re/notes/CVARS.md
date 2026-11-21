# Console variables

Image: default.xbe. Symbols: `g_pConsole` (006B38D8), `Console_CreateVariable` (001FD9E0),
`Console_RegisterFloat` (001FDCD0), `Console_RegisterInt` (001FDED0), `Engine3D_RegisterCVars`
(00481B30). Found 2026-10-16 from the variable names in `.rdata` and their one reference each.

## Registration

FCI keeps CryEngine's console variables, but this Xbox build has no console to type into and no
config file is read. Every variable is still registered by name:

- `Console_RegisterFloat` and `Console_RegisterInt` (eax = console; name, storage, default,
  flags, help; stdcall, `ret 0x14`). The default is a float for both. They look the name up in
  the console's map (a tree at console+0x88, entries keyed by a 0x1C-byte string, the variable
  object at entry+0x28) and return the variable's current value. The caller stores it in its own
  storage and reads that storage from then on. 16 float and 75 int registrations.
- `Console_CreateVariable` (thiscall; name, default string, flags, help) returns a variable
  object that keeps its own value; the caller holds the object (192 calls: `r_*` display
  settings, game options).

`Engine3D_RegisterCVars` registers the 3D engine's 80 `e_*` variables into one block
(this+0xC to this+0x104). The ones that bear on draw distance and detail:

| Variable | Kind | Default | Meaning (CryEngine 1) |
|---|---|---|---|
| `e_obj_view_dist_ratio` | float | 48 | how far objects are drawn, per object size |
| `e_obj_lod_ratio` | float | 8 | when objects change to simpler models |
| `e_obj_sprite_switch_dist_ratio` | float | 7.2 | when vegetation changes to sprites |
| `e_max_view_distance` | float | 0 | the far limit (0: the level's own) |
| `e_detail_objects`, `e_vegetation`, `e_vegetation_sprites` | int | 1 | grass, vegetation, sprites on |
| `e_shadow_maps` | int | 0 | per-object shadow maps |
| `e_max_shadow_map_size` | int | 512 | the largest shadow map the engine asks for |
| `e_max_entity_lights` | int | 4 | lights per entity |

Defaults as registered (`RECOMP_CVAR_LIST=1`, the intro, 2026-10-16: 91 variables through the two
pointer functions; the 3D engine's block is at 0100F340..0100F444 in that run).

## Setting them (fci_cvars.c)

Prehooks on both register functions record each name with its storage. When the options hold
`RECOMP_CVAR_<name>=<value>` the prehook replaces the default argument, so the console creates
the variable with that value and the caller stores it. While the game runs, the control command
`set RECOMP_CVAR_<name>=<value>` writes the recorded storage. A prehook on `Console_CreateVariable`
(Evolution 0017ED53) replaces its default string with `RECOMP_CVAR_<name>` (a copy in guest memory,
2026-10-22) and lists the string defaults with `RECOMP_CVAR_LIST=1`; the control `set` does not reach
these, they keep no storage of their own outside the variable object.

Checks (intro fly-over, render scale 2, 2026-10-16):

- `e_obj_view_dist_ratio=200`, `e_obj_lod_ratio=40`, `e_obj_sprite_switch_dist_ratio=40` from the
  options: the log shows each default replaced (48, 8, 7.2). The distant islands then show
  full-detail palms instead of the simpler ones, and bushes, plants and a rock that were not drawn
  at all. Draws per frame about 400 -> 750; the intro still holds the title's 30 frames/s.
  100 / 16 / 15: about 600 draws, also 30 frames/s.
- The same three through the control `set` while the intro runs: draws per frame rose from about
  600 to 900 within a second, and fell to about 700 a second after the defaults were set back (the
  camera moves, so the counts are indicative).
- `e_shadow_maps=1` with `e_max_shadow_map_size=2048` runs without a stop, with no visible change
  outside the tunnel (the palm shadow is the sun shadow map, GRAPHICS r4).

The map editor (ige.xbe) registers the same variables through the same functions built as thiscall
(Console_RegisterFloat 0006A1D0, Console_RegisterInt 0006A380, console at 00649788); its image
profile hooks those (MAP-EDITOR r4).

## r_glow (bloom)

The screen-wide bloom. Both titles register it through `Console_CreateVariable` (a string-default
variable that keeps its own value inside the returned object), and both store the object in the
3D engine. Default is `"1"` (on).

### Creation (default, `default.xbe`)

`sub_00057E90` (recomp_0005.c), label `loc_00058B98` (line 28987). One of the engine's `r_*`
registrations, run from `sub_00048370` ← `sub_001FCCB0` (the level/engine init that stores the
engine at **0x6B3984**, a 0x3DC-byte object):

```
PUSH32(esp, 0x5F9408);   # help  "Enables glow. Default is 1.Usage: r_glow 0/1"
PUSH32(esp, 0);           # flags = 0
PUSH32(esp, 0x5FB390);   # default string "1"
PUSH32(esp, 0x5F9400);   # name  "r_glow"
ecx = ebx;                # console (0x6B38D8 = g_pConsole)
MEM32(edi + 0x174) = eax; # the variable object -> engine + 0x174
call sub_001FD9E0;        # Console_CreateVariable
```

So **r_glow's variable object lives at `engine+0x174` = `0x6B3984+0x174`**.

### Creation (Evolution)

`sub_0004405E` (recomp_0006.c), label `loc_00044CA1` (line 12175), run from `sub_00039E64`
(the level/engine init). Console is **0x56B4E4**:

```
PUSH32(esp, 0x4AD268);   # help
PUSH32(esp, ebp);        # flags = 0
PUSH32(esp, ebx);        # default string "1"
PUSH32(esp, 0x4AD260);   # name  "r_glow"
ecx = edi;                # console (0x56B4E4)
MEM32(esi + 0x178) = eax; # the variable object -> engine + 0x178
call sub_0017ED53;        # Console_CreateVariable
```

**r_glow's variable object lives at `engine+0x178`** in Evolution (the engine block's offsets
differ from the default title).

### `Console_CreateVariable` and the variable object

- default `sub_001FD9E0` (001FD9E0–001FDBF3, recomp_0029.c:34428); Evolution `sub_0017ED53`.
  Both thiscall, `ecx` = console, stack `name, default string, flags, help`; both build the
  console map at `console+0x88`.
- Variable object layout (default constructor `sub_001FF910`, recomp_0030.c:3092):
  `+0x00` vtable (0x6139D8), `+0x04` type/flags, `+0x08` name, **`+0x10` pointer to the int
  value**, **`+0x14` pointer to the float value**, `+0x60` 0. The constructor writes the int
  from the float (`*[var+0x10] = *[var+0x14]`), so the int slot is the canonical 0/1.
- **Reading r_glow**: `var = [engine+0x174];  on = [[var+0x10]]` (int). Float form `[[var+0x14]]`.

### What it gates

The native bloom post-effect. The effect loads `Shaders\\PostEffects\\Glow.psh` (0x0061A924) in
`sub_0025B320` (recomp_0036.c:7094); the setup chain is `sub_0023ACA0` → `sub_00250610` →
`sub_0025B320`. The per-frame glow uniforms are the `Glow*` parameters: `sub_00471440`
(`GlowBoostLevel`, `GlowIntensitySpeed`, `Glow`), `sub_0047AEE0` (`GlowBias`, `GlowIntensity`,
`GlowBrightness`, `UseGlowParams`), and `sub_000B2040` (`SetGlowParams`). `r_glow 0` turns off
this Glow.psh pass (the bright-parts downsample/blur chain and its composite into the scene).

The exact per-frame reader of the engine's r_glow slot is not statically resolved: the two-step
CVar read idiom (`[[x+0x174]+0x10]`) occurs in only three functions (`sub_001D99C0` on a
different 0x1D8-byte object at 0x6B38F4, and `sub_003FC680` / `sub_003FFC60`, both float, in
recomp_0063.c) — none on `0x6B3984+0x174`. The renderer therefore reads r_glow through the
CVar's virtual accessor or a value latched at level load, not an inline two-step read.

### Setting it (the hook already exists)

The `Console_CreateVariable` prehook in `src/fci_cvars.c` (see "Setting them" above) already
replaces the default string with `RECOMP_CVAR_<name>`. So **`RECOMP_CVAR_r_glow=0` in the options
turns bloom off**: the prehook swaps the `"1"` default for `"0"` and the console creates r_glow
with value 0. As with the other `Console_CreateVariable` variables, the runtime `set` does not
reach these (they keep no storage outside the object) — only the options default at level load.

For a runtime toggle, record the returned object in the prehook when `name == "r_glow"` and write
`*([var+0x10]) = 0` (int) (untested: whether a live write takes effect depends on that reader).

### Overlap with the project's own renderer

`src/xbr/fci_enhance.cpp` has a **separate** glow: the volumetric-fog glow (`glowdown` /
`glowblur`, the scene's bright parts at an eighth, scattered in the fog), gated by
`RECOMP_FCI_XBR_FOG_GLOW` (default 0.5). It is not the game's `r_glow` and is not touched by it.
`r_glow 0` removes the game's native screen-wide bloom; the project's fog glow stays. The project
does not replace the game's Glow.psh pass, it adds its own fog glow over the picture.

Open: the `r_*` variables (Console_CreateVariable), e.g. `r_eyeadapt`, are handled the same way
as `r_glow` above (options default replacement through the existing prehook).

Measured 2026-11-20 (Instincts, Training quick save looking at the sky, runs b92-glow1-094601 and
b92-glow0-094629): RECOMP_CVAR_r_glow=0 logs "cvar r_glow = 0 (default "1")" and removes the soft glow
around the sky and bright leaves (frame mean difference 6.7/6.8/10.5 per channel). fci.ini `gfx-bloom` sets
it; the settings menu's BLOOM row saves it for the next start (the variable is created at engine start).
