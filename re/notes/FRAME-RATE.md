# Frame rate

How the engine times its frames, for running it above the Xbox's 30 frames a second.

## Where 30 comes from

Both games create the device with a presentation interval of 0x80000002: every second vertical blank.
The host honours it by waiting out the period after each present (`app::Pace`, src/xbr/fci_xbr_glue.cpp);
`RECOMP_FCI_XBR_UNLOCK=1` drops the wait. Nothing in the game itself caps the rate:

- `g_maxfps`, the game's own limit, defaults to 500 ("Sets the maximum frame rate", Instincts
  00057E90 registers it, its handle at game+0x74).
- Checked 2026-10-22 (Instincts, the Training tunnel save): unlocked, the tunnel ran about 90 frames a
  second; holding the stick forward for one second covered the same distance locked and unlocked.

## The timer

A global timer object, Instincts 00644488, Evolution 0056CCC8 (the same layout):

| Offset | |
|---|---|
| +0x00 | frames counted |
| +0x04 | the clock function (virtual) |
| +0x08 | the clock's base |
| +0x10 | the previous frame's time |
| +0x20 | this frame's time |
| +0x28 | the game's clock: frame times added up while the game runs (not while paused) |
| +0x30 | this frame's time minus the previous one's |
| +0x68 | the frame time the game steps by |

- Update (Instincts 001FF7C0, Evolution 001801EB), once per game frame from the game update
  (Instincts 001FCD40, Evolution 0017E185). A frame time over 0.1 s (the double at Instincts 005F3688) is
  cut to 0.1 s and the base moves on by the rest: a long hitch slows the game rather than jumping it.
  Shorter frames are used as measured; there is no fixed step.
- Frame time (Instincts 001FF840, Evolution 00180281): the double at +0x68 as a float, or 0 while the
  game flags (+0x3C0, +0x329, +0x60 of [006B3984]) say it is not running: paused or loading.
- The game update hands the clock and the frame time to the scripts as `_time` and `_frametime`
  (Instincts 00215C10, Evolution 0018FA11; the third global, beside them, takes an integer).

## Measured

Instincts, the Training tunnel save, the stick held forward 2 s (tools/frametrace.py, 2026-10-22):

| Paced at | Game frame time (median) | Distance | Speed in 0.25 s steps from the start |
|---|---|---|---|
| 30 | 33 ms | 8.65 m | 3.57, 5.08, 5.12, 4.96, (the tunnel's dip) 0.51, 2.54, 5.36 |
| 60 | 17 ms | 8.37 m | 3.11, 5.10, 5.12, 4.78, 0.47, 2.38, 4.96 |
| 120 | 13 ms | 8.29 m | 2.98, 5.09, 5.12, 4.67, 0.48, 1.97, 4.52 |

The walking speed is the same; the speed-up is slower the higher the frame rate (a step per frame not
scaled fully by the frame time). This host reaches about 80 frames a second in the tunnel.

## Known exceptions

- The player's acceleration (above).

- Bink videos: the game advances a video a frame per present. The files give their own rates (header
  frames per second: 15, 25, 29.97, 30.07), so at another rate they play at the wrong speed.
- Physics sub-steps: `max_time_step`, `max_time_step_vehicle` and `p_fixed_timestep` (console variables)
  are not yet looked at, nor the paraglider's `game_Glider*` values.

## The trace

`RECOMP_FCI_TRACE=<file>` (src/fci_trace.c) writes a CSV line per 3D frame from the Camera_BuildViews
prehook: the host time, the timer's +0x28 and +0x68, the player's position and the ViewInput angles
(pitch, roll, yaw); tools/frametrace.py summarises and compares traces. The position is the player
entity's: Player_ProcessInput's `this` for the game view's block is the player's actor, its +4 the
entity, the entity's +0x34C its position (Instincts, found 2026-10-22 by diffing memory dumps around a
walk: (744.92, 217.89, 19.48) to (739.97, 219.15, 19.31) in 1.5 s; the actor's +0x224 is about 1.4 m
higher, the eyes). The view's camera copies (view+0x1468) are rebuilt each frame and are no position source.
