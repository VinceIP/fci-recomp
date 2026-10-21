# Movies

Image: default.xbe. Symbols: `Movie_Play` (00220200), `Boot_PlayLogos` (001FD760), `sprintf`
(004EAAFB), `g_strDataRoot` (00644524).

## Playing a movie

`Movie_Play` (cdecl, first argument a path) plays a Bink file from start to end and returns; while
it runs it polls the controllers (`Input_PollPads`). Callers build the path first:
`sprintf(buffer, "%s%s", g_strDataRoot, "Videos\\<name>.bik")`.

| Movie | Played by | Length on this build |
|---|---|---|
| `Ubisoft.bik`, `Crytek.bik` | `Boot_PlayLogos` | to about 5 s and 17 s after start |
| `Intro_FCI.bik` | 00048F57 (startup, after the logos) | to about 31 s |
| `StartMenu_boat.bik` | the title screen's background (not through `Movie_Play` at this point) | loops |

Pressing A or Start does not end the logos or the intro (presses every second from 3 s did not
change when each movie opened).

## Skipping (RECOMP_FCI_SKIP_INTROS, RECOMP_FCI_SKIP_MOVIES)

`Movie_Play` is gated (tools/compile.py `REPLACE`, kind `gate`; `fci_gate_00220200` in
src/fci_disk.c): a listed movie returns at once with eax 0 (callers ignore the result),
and the title goes on as if it had played. With the three startup movies skipped the title screen
is up 8 s after start instead of about 31 s.

Tried first and dropped: failing the movie's file open (path redirected to a missing file). The
title then waited for good after the failed open of `Ubisoft.bik`.

## Far Cry Instincts Evolution (default.xbe, data/evolution)

Found 2026-10-20 from the `Videos\*.bik` strings and their references.

- The movie player is 00196D41 (cdecl: path, then four flags), called right after
  `sprintf(buffer, "%s%s", data root, "Videos\<name>.bik")` (0039CBB5), as Instincts' `Movie_Play`.
  It clears the event at 004EEB34 when it starts and sets it after about 70 frames, or at once when
  the file does not open (that error path returns cleanly).
- Startup: `Ubisoft.bik` (0017EB68; there is no Crytek logo) and `Intro_FCI.bik` (00039E64).
- `Attract_Mode.bik` (000FAF1E). The title page's Start handler (000FAFC7, `B_Start`, actions 0x33 and
  0x2F) and the title page's entry (000FABAC, when [[0056B594]+0x2FA] is set: back from the game)
  both go through 000FAF1E, which plays the video only while `WaitForSingleObject([004EEB30], 0)`
  times out (a manual-reset event cleared while a background load runs; 0003C828 loops on it with a
  loading screen) and then moves on (00122774). After Quit to Main Menu (a relaunch into default.xbe)
  the load is still running, so the video plays in full, about 100 s, before the main menu.
- A second player, 0019674C (path first), opens the menus' background videos
  (`StartMenu_boat.bik`, `MenusBack_sun.bik`) and `PREY.bik` (000F9FF7).
- The Special Features menu (000FB2FF) plays `INTRO`, `FCI_02`, `PREDATOR`, `MULTI`,
  `PREDATOR_MULTI`, `MAP_EDITOR`, `Previews_GR3_ntsc` and `Previews_R6L`.

Skipping: 00196D41 is gated by the same `fci_gate_00220200` (data/evolution/default.profile.json).
`RECOMP_FCI_SKIP_INTROS` skips Ubisoft, Intro_FCI and Attract_Mode. For Attract_Mode the gate first
waits on the event (profile constants `FCI_ATTRACT_WAIT` 0035C128, `FCI_ATTRACT_EVENT` 004EEB30) through
the game's own wait, so the title goes on as after a short video. Checked (regression scenario
`quit-menu`, 2026-10-20): Quit to Main Menu reaches the main menu about 4 s after the relaunch.
