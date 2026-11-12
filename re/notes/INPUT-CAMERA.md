# Input and camera

Image: default.xbe. Names are in `re/symbols/default.xbe.json` (functions, the `GameView`,
`ViewInput`, `InputManager` and `Camera` structs). Found 2026-10-15 with memory dumps around stick
turns, data watches (write and read) on the fields involved, and the disassembly.

## From the stick to the view angles

One frame, in order:

1. `Game_FrameUpdate` calls `Input_PollPads(g_pInputManager, 1)`. For each port with an open
   handle it reads the pad with the XDK's `XInputGetState`. Then `Input_ProcessStick` writes each
   stick as two floats into the `InputManager` (`leftX/leftY/rightX/rightY`, one per port). The
   test controller is on port 2.
2. `Camera_BuildViews(view)` runs the action map: `Controls_Update([view+0x74C], time)`, then
   `ActionMap_Dispatch`. For each binding, `ActionMap_ReadControl` reads the control:
   - Codes 0x15000000 to 0x18000000 are the stick axes. Right Y is multiplied by [0062A6E0]
     unless the invert flag is set.
   - Each value is queued as (action id, value) and handed to the listener, which is the game view
     itself: `GameView_OnAction` (vtable 00604BD8) calls `GameView_HandleAction`.
   - The data watch found no other reader of the stick floats (the menus read them separately
     through 000674E0).
3. `GameView_HandleAction` switches on the action id. Look X and Y go to `view+0x5DC` and
   `view+0x5E0` and raise input flags 7 and 8 in the view's `ViewInput` block (`view+0x60C`).
4. `Look_Update` / `Look_ComputeTurn` turn the look values into this frame's turn, in degrees:
   - The values are kept only while flags 7 and 8 are set.
   - The smaller axis is snapped when the other dominates.
   - The values are clamped into `ViewInput.lookAxes`.
   - Then the target-dependent slowdown and the turn rate are applied (the per-frame time comes
     from 001FF840).
   - The result goes to `ViewInput.turnYaw` (+0xBC) and `turnPitch` (+0xC0).
5. Camera_BuildViews looks up the controlled entity (`view+0x30`) and its actor component, and
   calls two of the actor's virtuals with the block:
   - +0xD0 is `Player_ProcessInput`. It starts from the block's view angles (`ViewInput.angles`,
     +0x18: pitch, roll, yaw in degrees) and builds delta = (turnPitch, 0, -turnYaw).
     `Player_ApplyTurn` adds the delta and wraps each angle to +-180 (`Angle_Wrap180`). When the
     player is mounted (player+0x400/+0x401) it uses a limit helper instead. The turned angles
     are written back.
   - +0xCC is 0008C840, the other reader of the turn.
   - On foot the pitch ends up clamped to +-85 degrees.
6. Camera_BuildViews clears the block and builds the cameras. Each view's `Camera` (0x63C bytes)
   is copied with `Camera_Copy` into `view+0x1468+i*0x63C`, which the renderer reads. These are
   copies: the heap addresses with the camera's yaw (and the static 00518214) are written from
   these and other copies. They are not the source.

Signs: yaw falls when turning right (turnYaw > 0), and pitch grows looking down (turnPitch > 0).
On a full stick deflection the turn is about 166 degrees per second, after a ramp-up.

## Direct mouse aim

The host's 1:1 mouse aim (src/fci_aim.c, 2026-10-15) uses a prehook on
`Player_ProcessInput`:

- **What it adds.** The mouse counts since the last frame times degrees per count go into
  turnYaw/turnPitch, on top of whatever the stick gave.
- **Exactness.** The counts are applied once per game frame as an angle, so the turn is exact at
  any frame rate. The stick's curve, acceleration and slowdown are bypassed.
- **Which block.** The prehook only touches the block whose address is `view + 0x60C`. The view is
  recorded by the Camera_BuildViews prehook, so other actors' blocks are never changed.
- **Pitch limit.** It caps the pitch change so the new pitch stays within +-90. Otherwise the
  game's wrap to +-180 would turn a fast upward flick into the downward limit.
- **Fallback.** When the step has not run for 250 ms (menus, vehicles, cutscenes, the editor),
  fci_input.c drives the stick from mouse speed instead.

Checks (tools/game_driver.py `mouse:dx:dy`, which writes "mouse dx dy" to RECOMP_FCI_CONTROL):

- 1000 counts at 0.05 turned the yaw by -50.000.
- 200 counts down added +10.000 of pitch.
- Ten moves of 100 added up exactly.
- 7200 counts turn the view a full 360 degrees and bring back the same frame (regression
  scenario `mouse-aim`).
- 6000 counts up stop at -85.

## Open

- What 0008C840 does with the turn (another view or weapon sway?).
- How the 85-degree pitch clamp is applied, and where. It is not in Player_ApplyTurn's unmounted
  path.
- What the vehicle actors use in place of `Player_ProcessInput` (direct aim falls back to the
  stick there).

## Aim assist (cl_aimingHelper)

The game's aim helper is one console variable, read every frame by the look code; fci_aim.c turns it off
in multiplayer while mouse and keyboard are in use (2026-11-12, read in the lifts).

| | Instincts | Evolution |
|---|---|---|
| Created | `Console_CreateVariable` (001FD9E0) with name 005F9B44, stored at settings+0x9C (`recomp_0005.c`, the store after the call) | 0017ED53 with name 004AD9A4, stored at settings+0xA0 (`recomp_0006.c:10806`) |
| Settings object | `MEM32(006B3984)` | `MEM32(0056B594)` |
| Read | `Look_ComputeTurn` 0003D450: `MEM32(MEM32(cvar+0x10))`, 0 skips the scaling (`recomp_0003.c:19950`) | 00030CB2 at 00030F8B, same chain (`recomp_0003.c:17527`) |

The cvar's +0x10 points at its int. 0008C840 (above) does not read the cvar; whether any part of the assist
escapes it is untested in play. Each player's game applies its own assist; nothing of it is sent.

