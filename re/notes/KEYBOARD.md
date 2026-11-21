# The on-screen keyboard: typing on the PC keyboard

Images: default.xbe of each title. Found 2026-11-12 in the lifted code and checked with driver runs (frames
and stderr) on copies of the game.

The on-screen keyboard (profile names, cheat codes) is one object for the whole game, reached through a
pointer. While it is open, `fci_vkb.c` turns typed characters, Backspace, Enter and Esc into calls of the
keyboard's own functions, as its pad buttons make them.

| Item | Instincts | Evolution |
|---|---|---|
| keyboard object pointer | `[0x6B3250]` (passed by every open call) | `[0x56ADCC]` |
| open / close | `sub_0010B500` / `sub_0010B5E0` | `sub_000D0750` / `sub_000D07EC` |
| button dispatch (vtable slot 5) | `sub_0010A1A0` | `sub_000CFDEF` |
| insert a string at the cursor (X's space) | `sub_0010BFF0`, eax keyboard, edi string | `sub_000D0DFB`, eax keyboard, string on the stack |
| delete (Y) | `sub_0010C0B0`, eax keyboard | `sub_000D0E83`, eax keyboard |
| accept (A, letter wheel on its first slot) | `sub_0010BBD0`, eax keyboard | `sub_000D0C0B`, eax keyboard |
| back (B) | `sub_0010B7D0`, esi keyboard | `sub_000D0940`, esi keyboard |
| UI input router (manager's vtable slot 1) | `sub_000675B0` | `sub_0005022C` (function range 0x5022C-0x508CF in the table at 0x4AF750) |
| popup input root in the manager | `+0x20` | `+0x24` |

Object fields used: the callback at `+0x58` (set while open), the letter wheel at `+0xAC` (its slot at
`+0x18`). A typed character is inserted as a one-character wide string object (inline buffer: character
at +4, length 1 at +0x14, capacity 7 at +0x18), the way X inserts its space. Enter presses A with the
wheel's slot set to 0 (the accept slot) and puts the slot back if the keyboard stays open.

## Open and focus

Before the first open, Instincts' object holds leftovers at `+0x58`, so the callback alone does not say
the keyboard is open: prehooks on the open and close functions track it.

A popup over the keyboard (the "enter at least one character" warning after accepting an empty text)
takes all pad input. The UI input manager's router (Instincts `sub_000675B0`, called once a frame through
the manager's vtable from the game loop `sub_001FCD40`) reads the popup root at `[manager+0x20]`: when set,
it delivers input only to it (call `sub_00353DC0` at 0x67AB6), otherwise to the current screen (0x67AD4).
Evolution's router reads it at `+0x24` (0x506CE). Measured on Instincts with prehooks: the keyboard's
dispatch runs for pad presses while it has focus and not for the press that dismisses the popup, and the
popup's root (0x8000DD20) replaced the keyboard screen's (0x8000DE40) as the routed one.

Typing therefore needs: opened, callback set, and no popup root. Keys typed while a popup is up are
dropped, and keys then go to the pad bindings as on any menu.

Checked (copies, scripted runs): Instincts profile "Vince Q2" typed, Backspace and Enter, profile created;
cheat keyboard with Enter on empty text, "abc" typed under the popup ignored, "xy" typed after OK, Esc back
to the main menu; typing on the main menu with no keyboard open does nothing. Evolution: the same profile
and popup runs.

## The map editor (ige.xbe)

Found 2026-11-13 in the editors' lifted code and checked with driver runs on copies. Save Map opens the
keyboard twice: the map's name, then its description. It is the game's class, with two differences:
every method takes the keyboard in ecx (thiscall), and its dispatch is at vtable+0x28. Callback +0x58,
letter wheel +0xAC (slot +0x18) and the insert's string layout are the game's.

| Item | Instincts editor | Evolution editor |
|---|---|---|
| keyboard object pointer | `[0x64AFC0]` (0x104 bytes) | `[0x67863C]` (0x114 bytes) |
| open / close | `sub_000B56E0` / `sub_000B4B20` | `sub_000F10A0` / `sub_000F04D0` |
| insert a string (X's space), string on the stack, ret 4 | `sub_000B5340` | `sub_000F0D00` |
| delete (Y) / accept (A) / back (B) | `sub_000B5430` / `sub_000B58D0` / `sub_000B4B30` | `sub_000F0DF0` / `sub_000F1310` / `sub_000F04E0` |
| pad poll (handles at this+0x248, XInputGetState) | `sub_00061320` (XInputGetState 005A95A2) | `sub_0009BE30` (XInputGetState 005CFB02) |

The editors' pad poll is hooked as the game's Input_PollPads (the typing runs there each frame, and the
driver's poll count now counts in the editor too). The input router is not hooked, so typing does not
stop under a popup in the editor. Checked: both editors, Save Map: three Backspaces and "Hill X1" made
"Player Hill X1", Enter went on to the description, Esc came back to the name with the text kept.

The one-character string is built zeroed, so its first word (the string class's vtable in the game's
layout) is 0: both titles' insert functions read the buffer, length and capacity directly and never
dispatch through it. A title whose insert did would need the class's vtable written in.
