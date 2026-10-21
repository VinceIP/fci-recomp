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
  loading screen) and then moves on (00122774) whether or not the load is done.
  The load it covers is the loader thread's (0017E811), which copies the music streams (`*.ss2`) from
  the disc into the Z: cache (001F0742: `T:\$u%s%s` or `D:%s%s` to `Z:%s%s`, when 001F0510 finds the
  copy missing or its time or size different; 001F106B copies and stamps the copy with the disc file's
  time). Until runtime patch 0020 that stamp failed, every boot copied again while the menu music
  streamed from the copy, the copy's open failed on a sharing violation and the loader stopped in the
  game's fatal error screen (0018FCC2/0018FAD7: draw the message, then `jmp $`): the event stayed clear
  and the video played after every Start and every Quit to Main Menu (and the spinning thread cost the
  menus a third of their frame rate). With the loader done, Start goes straight on.
- A second player, 0019674C (path first), opens the menus' background videos
  (`StartMenu_boat.bik`, `MenusBack_sun.bik`) and `PREY.bik` (000F9FF7).
- The Special Features menu (000FB2FF) plays `INTRO`, `FCI_02`, `PREDATOR`, `MULTI`,
  `PREDATOR_MULTI`, `MAP_EDITOR`, `Previews_GR3_ntsc` and `Previews_R6L`.

Skipping: 00196D41 is gated by the same `fci_gate_00220200` (data/evolution/default.profile.json).
`RECOMP_FCI_SKIP_INTROS` skips Ubisoft, Intro_FCI and Attract_Mode, the last as if the video had ended
at once: 000FAF1E goes on, and 0003C828's loading screen covers a load still running. (An earlier gate
waited for the event first; with the loader stopped, that hung Start for good.) Checked 2026-10-21: Start
pressed as the title appears reaches the main menu, on a fresh save and on a save with an old cache;
regression scenarios `menu` (the cache copies' times) and `quit-menu`. A cache written before patch 0020
keeps its wrong times, and the copy of a stream already playing still fails: such a cache folder has to
go once (the game fills Z: again). On a first boot the menu music's streamer opens its copy while the
loader still writes it and fails once on a sharing violation, without harm (Instincts does the same).
