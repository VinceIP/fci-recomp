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

Open: the `r_*` variables (Console_CreateVariable), e.g. `r_glow` and `r_eyeadapt`, would need
the console's map (console+0x88) walked or the returned objects recorded.
