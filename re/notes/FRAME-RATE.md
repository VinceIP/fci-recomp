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

Evolution, the first level from a new game: the player rides the jeep's mounted gun along a scripted
drive starting about 18 s of game time in. The player entity follows the jeep, so its position compared at
the same game time (2026-10-22, paced at 30 and 120):

| Game time | 18 s | 20 s | 24 s | 28 s | 32 s | 36 s | 40 s |
|---|---|---|---|---|---|---|---|
| Apart | 0.7 m | 5.0 m | 11.4 m | 15.0 m | 31.3 m | 51.7 m | 65.1 m |

At 120 the jeep is faster from the start (25.1 m against 20.6 m after 2 s), then near 34 s it nearly stops
while the 30 run drives on: the vehicle's motion depends on the frame rate.

The distance it covers from 17 s to 26 s of game time: 96.5 m at 30, 104.9 m at 45, 106.1 m at 60,
107.6 m at 90, 109.7 m at 120. The vehicle scripts (lua.dat, compiled) set `max_time_step_vehicle` 0.02
(the characters' `max_time_step` 0.025), so a frame of 33 ms takes two physics steps, well inside
`p_max_substeps` 5, and 60 steps the jeep once per 17 ms frame, about the size of 30's two steps; yet 60
goes 10% further. Not the physics step: something the driving does once per frame (the AI's throttle or
speed target are the suspects).

### The jeep, 2026-10-22 (later)

How the AI drives (Evolution addresses, Instincts' in brackets):

- The AI's vehicle driver 0007F99B (000A38F0; its vehicle controller's +0x9C is the vehicle) builds a
  command of digital actions each frame (AddAction 0005AEB2 [00077200]; CheckAction 0005AF2C [000772A0];
  a bit array from the command's +4): 3 forward, 0xD brake, 1/2 full steering lock, 0x22 handbrake. It
  compares the speed with the one it wants and presses forward or brake, so speed is held by on/off control.
- The movement handler 0012C298 (0019E5A0) turns the actions into the vehicle's bits at +0x54 (3: 0x01,
  4: 0x02, 1: 0x04, 2: 0x08, 0xD: 0x10, 0x22: 0x20, 0xA: 0x40) and calls the type's drive; the jeep's is
  0012D981. It sets the pedal straight from the bits (1, 0, -1; braking pushes against the motion), the
  steering to the full lock for the speed (`max_steer_v0 + max_steer_kv * speed`), and relaxes it by
  `(steer_relaxation_v0 + kv * speed) * frame time` without a bit; the parameters come from the
  vehicle script's SetDrivingParameters (000A38CB, the vehicle's +0x58...). Frame time is capped at 0.1 s.
  It is frame-rate correct.
- With no movement target the AI driver adds only the handbrake. Between waypoints that happens for exactly
  two frames at any frame rate (7 pulses in the ride, 2 frames each at 30, 45, 60): 67 ms of handbrake on
  the Xbox, 33 ms at 60. The jeep loses less speed in each and runs ahead. Holding such a pulse for two
  30 fps frames' time brought the ride from 17 s to 26 s to 102.4 m at 60 (106.2 m without; 96.7 m at 30);
  the hold was a band-aid and is removed. The engine update cap (fci.ini engine-update-cap) keeps the game
  at 30 instead; uncapped, the difference stays.

What remains is the physics. RECOMP_FCI_VEHTRACE (src/fci_vehdiag.c) logs the car's bits each frame and
RECOMP_FCI_VEH_REPLAY drives the car by a trace's bits instead, by game time. The same bits from a 30 fps
run, replayed: at 30 the same 98.0 m (to the metre for 8 s); at 60 103.0 m; at 40, 45 and 50: 99.1, 102.8,
98.6 m. Straight acceleration is identical at every rate (21.3 m in the first 2 s); turning and braking
differ. The vehicle does substep: with `p_max_substeps` 1 the 30 fps replay covers 53 m (each 33 ms frame
cut to a 20 ms step), so 30 steps it 2 x 16.7 ms and 60 1 x 16.7 ms. Something in the physics happens once
per world step rather than per substep (wheel contacts, presumably). Matching it exactly needs the physics
world stepped at 30 a second, with the drawn positions interpolated in between; not attempted.

The drive also starts earlier at higher rates (game 17.89 s at 60, 18.01 s at 30): about seven frames of
something before the AI first presses forward, presumably the AI's goal pipeline advancing a step a frame.

### The physics world step at 30 (2026-10-23)

The game steps the physics world once a frame (Evolution: 0017E353, from the game update 0017E185, calls
00282B1C with edi = the world [0056B508] and the frame time times the time scale [[0056B594]+0x3E8], ret 4;
Instincts: 001FCF40 calls 003741E0 (world, time), ret 8, the time scale at [[006B3984]+0x3D0]). It is Far
Cry's CPhysicalWorld::TimeStep without the entity-type flags argument (so vehicles and characters cannot be
stepped at different rates without changes inside it): steps over 0.2 s or under 0 are refused, the time is
rounded to the granularity (world +0x3C) with the remainder carried in +0x20C (not in Far Cry). Each rigid
body splits the step into whole substeps (max_time_step), with contacts, engine, suspension and handbrake
per substep; the drive input (pedal, steering, handbrake) arrives once a frame as an action.

src/fci_physics.c gates the step: frame times add up and the world steps only when a whole 1/30 s is due
(RECOMP_FCI_PHYS_HZ=30; off by default, because without interpolated drawing everything that moves, the
player too, updates 30 times a second). The jeep, handbrake hold off, 17-26 s: 95.0 m at 30, 99.0 m at 60
(106.2 m stepping per frame), 104.4 m at 116. Better, not the same: the AI still decides once a frame. Its
two-frame pulses (the handbrake between waypoints) can now fall between two physics steps, and its drive
starts earlier (17.82 s at 116, 17.99 s at 30). Far Cry's AI (CryAISystem/Puppet.cpp) mixes real time
(m_fTimePassed) with per-update counts (sound timeouts drop 0.001 an update; goal pipes advance an op an
update; full and dry updates), so the AI is frame-counted in the original engine too.

Experiments that changed nothing: holding the vehicle's bits to 30 changes a second (the AI's on/off
decisions at the Xbox's rate): 105.2 m at 60 either way.

Physics variables (RECOMP_CVAR_LIST, Instincts): `p_fixed_timestep` 0 (no fixed step), `p_max_substeps`
5, `p_gravity_z` -9.8. The game's own variables (`max_time_step`, `max_time_step_vehicle`, `g_maxfps`,
`game_Glider*`) are registered through 001FD9E0 (a string default), which RECOMP_CVAR does not hook yet.

## Known exceptions

- The player's acceleration (above).

- Bink videos (header rates 15, 25, 29.97, 30.07): the menus' background videos of 25 and over advanced a
  frame per game frame; paced by their header's rate since 2026-10-22 (re/notes/MOVIES.md). Full-screen
  movies already waited on BinkWait.
- Vehicles: the AI's handbrake pulses (fixed, above) and the physics stepped once a frame (open).
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

## Recorded input

A test drives the game from a recording of someone playing it (src/fci_record.c, runtime patch 0021,
tools/record2script.py):

- `RECOMP_FCI_RECORD=<file>` logs each controller report the title reads when it changes, the direct
  mouse aim of each game frame, the moment the player's actor first appears (the sync) and, ten times a
  second, the state: frames, polls, videos, the game clock, the player, its entity's id, the id of what
  the view controls and the position.
- tools/record2script.py turns the recording into game_driver arguments (`@<stem>.steps`): the presses
  before the sync as live pad steps (gaps capped at 1.5 s), then `wait:ingame` and `wait:replay`; and a
  replay file of the rest, in game seconds from the sync.
- `RECOMP_FCI_PLAYBACK=<file>` replaces the input with the replay's from the sync on, by the game's clock,
  so the same input reaches the game at the same game time at any frame rate. Reports recorded while the
  clock stood still (a loading screen's "press A"; its clock value depends on the frame rate) wait for the
  clock to stand still for 250 ms and then for their recorded host time after the report before. With
  both options, the recording logs what the replay did. (`RECOMP_FCI_REPLAY` is fci_probe.c's.)
- The status file gains `game_s` and `replay_left` (game seconds of the replay still to come).

Checked 2026-10-22 on Instincts' tunnel save: a 10 s recording (walk, turn with the stick, jump, a mouse
flick) replayed at 30 and 120 fps ends within 0.2 m and 0.7 degrees of the recorded run.

What the view controls: `view+0x30` holds an entity id, not a pointer (1000000, 0x000F4240, for the
player in both games); the player's entity holds its own id at +0xA4 (found in memory dumps: Instincts on
foot, Evolution riding the jeep's gun, where the view still controls the player). A different id there
presumably means a vehicle being driven; not yet seen. The actor's +0x400 (re/notes/INPUT-CAMERA.md's
mounted flags) is no vehicle signal: 1 on foot in Instincts, 0 on the jeep in Evolution.
