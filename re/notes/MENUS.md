# Main menu: QUIT GAME

Images: default.xbe of each title, and `common.dat` (DAT2). Found 2026-11-12 by parsing the archive and the
lifted menu handlers, and checked with driver runs on unmodified installs (frames and stderr).

## The layout

The main menu is `magma/crypregame.mgb` in `common.dat`: Instincts 62654 bytes, Evolution 68737 bytes, both
starting `MAGMA`. An element ID is the CRC-32 of its name (`B_CheatsDebug` 0xE5B2EC52, `B_Quit` 0x8BCDF569).
The hidden `B_CheatsDebug` item has its ID at three offsets: the item above's wrap link (0x4C7 in both), the
record (Instincts 0xA24, Evolution 0xB05) and the record's packed copy (+0xF). The record holds three X, Y
pairs (u32 each, X in 16.16) at +0x30, +0x54 and +0x78.

| Item | Instincts X, Y | Evolution X, Y |
|---|---|---|
| visible items | 62, 175 to 350, every 35 | 48, 144 to 324, every 30 |
| `B_Cheats` | 62, 350 | 48, 294 |
| `B_SpecialFeatures` | 62, 280 | 48, 324 (hidden until unlocked) |
| `B_CheatsDebug` | 70, 380 | 64, 354 |

The runtime (`fci_disk.c`, `add_quit_item`) changes a read buffer that is exactly one of these files, with
the old ID and position at every site: the ID becomes `B_Quit` and the item lines up with the others
(Instincts 62, 385; Evolution 48, 354, the row under Special Features). Same size; the archive is not
written. The label and tooltip rows in the main menu string table (`B_CheatsDebug` "CHEATS (DEBUG)" and
"Debugging Cheats (DEBUG)") become "QUIT GAME" and "Exit the game", padded with spaces to the same length.
Evolution's `B_Quit` row "QUIT TO MENU" is in section `P_MPStartMenu` only, so there is no clash.

Once the ID is changed, the game shows the item (it hides `B_CheatsDebug` only), navigation works (up from
STORY MODE wraps to it) and the tooltip shows.

## The action

The main menu handlers compare the action name against their own items only (names resolved from the
string addresses each comparison loads):

- Instincts `sub_0014EFF0`: B_StoryMode, B_SplitScreen, B_Multiplayer, B_MapEditor, B_SystemLink,
  B_Options, B_Live, B_Cheats, B_SpecialFeatures, B_Credits.
- Evolution `sub_000FB0A2`: the same plus B_Tutorial and B_MapManager.

Neither has a `B_Quit` branch, so the body ignores it. The gates on both (`fci_menu.c`, `fci_lan_menu.c`)
read the action hash and on `B_Quit` post WM_CLOSE to the window: the same path as the window's close
button, which waits for a save in progress, leaves the match list and keeps the log. Runs quit-rt-fci-132650
and quit-rt-evo-133204: "QUIT GAME added", "QUIT GAME chosen, closing", exit code 0.

## Recomp settings (Controller Options)

Found 2026-11-18 from the lifted Options screens (both titles) and checked with driver runs (frames and
stderr). The runtime's `fci_settings_menu.c` adds the control settings (mouse and keyboard) after the
game's rows in Controller Options (Options → profile → Controller Options, and the pause menu's Options),
then a DISPLAY AND GRAPHICS row; A on it fills the same list with the display and graphics settings, each
group under a header row (DISPLAY, GRAPHICS), and the title reads GRAPHICS. fci.ini's controls-vibration
is left out: the game's own CONTROLLER VIBRATION row sits in the same list.

The screen is an option-list controller at screen+0x60: the options (COption pointers) at +0x18..+0x1C,
the list widget at +4 (selected row s16 at +0xC4), the help widget at +8. A COption is 0x80 bytes: help
wstring +04, name +20, class name (std::string) +3C, value index +58, two value-text vectors +5C and +6C
(the row and the value popup read +6C), disabled +7C; six virtual methods (destructor, visit, set index,
load, can-previous, can-next). Ours use the game's methods with a `ret 4` no-op as visit, so the screen's
own apply visitor leaves them alone.

| Role | Instincts | Evolution |
|---|---|---|
| Controller Options fill (screen; ret 4) | `sub_00182570` | `sub_00119C3D` |
| A on a row (event 0x2F) | `sub_00152AF0` (eax event, list on the stack; ret 4) | `sub_000FDC7B` (eax event, ecx list; ret) |
| rows from the options ((list); ret 4) | `sub_00152510` | `sub_000FD74D` |
| set value index (ecx option; ret 4) | `sub_00151AB0` | `sub_000FCF84` |
| value popup input (ret 8) | `sub_00151FD0` | `sub_000FD37D` |
| clear / add / row value / help | `00152430` / `001521B0` / `00152730` / `00152800` | `000FD6DE` / `000FD4C1` / `000FD886` / `000FD901` |
| operator new | `0033CA60` (eax size) | `001FEACC` (cdecl size, 0) |
| title: child by name, wstring assign | `000665A0`, `00068750` | `0004F7FC`, `00050EC4` |

Gates: the fill adds our control rows and the entry row after the game's; A on the entry clears the list
and adds the header rows and one COption per setting (the game's handler is skipped); after the game's own toggle, rows rebuild and set-index, a
changed index is applied live (graphics options and engine variables) and written to fci.ini, or saved
for the next start. The title element `B_TitleButton` reads GRAPHICS while that page is shown, and is
put back after the value popup closes. Leaving with B returns to the profile screen; the next visit shows
the game's own options again.

Runs: Instincts q75-set1-090436 (pause menu) and q75main1-084226 (main menu); Evolution q75-evoset-085704 and -090042
(the second read the first's fci.ini: anti-aliasing OFF, toggled back ON). Known: on opening, the help
box keeps the entry's text until the cursor moves (the help call runs with row 0; something later
restores it, not found). Layout of 2026-11-20 (runs b90layout-082916, Evolution b90evo-084109: 5 control
rows, 20 display and graphics rows on both titles): the header rows are plain rows with one empty value;
the cursor stops on them (the disabled field +7C changes neither look nor cursor).

### GRAPHICS on the profile and pause options screens (2026-11-20)

The profile screen (P_EditProfile, magma/cryprofile.mgb: common.dat 0x55F7FF, 65,434 bytes, common.fat
entry 364, hash 0x64D56760) has no spare button: CHARACTER SETUP is in use (frame b90prof-091001). Its
button records each hold the id, four (x 0x3C, y u16, float 1.0) position sub-records, a UTF-16 placeholder
text and the tail `01 27 5A 4D 03 00 01 <up id>`; the next `01 <id>` is both the down link and the next
record's id. Focus is that explicit cycle; the screen's child count is the byte at 0x1B90 (10).

fci_disk.c adds a sixth button in the read hook: common.fat is read as an 8-byte header and then one
12-byte entry per read (probed), so the entry (0x55F7FF, 65434, hash) gets 65434 + 232; the 65,666-byte read
of common.dat is then rebuilt: DISPLAY's record (0x20CE-0x21B6) copied after itself with id 0xE87CB6E7
(CRC-32 of B_ControllerScheme, an unused P_EditProfile string-table entry relabelled GRAPHICS), its up link
DISPLAY, RESET DEFAULTS' up link (0x22AE) GRAPHICS, count 11, y 0xF5 -> 0x118 for GRAPHICS and 0x118 ->
0x13B for RESET DEFAULTS. Each step checks the original bytes first. A on it reaches the screen's
dispatcher sub_001937B0 (eax event, ecx screen): the gate swaps the action to B_ControlSetup's (0x7765E2D2),
so the game opens Controller Options, and flags our page; Controller Options' show sub_00181F70 fills
(our page instead of the controls) and relabels the screen, so the title GRAPHICS and the help line are set
after it. B returns to the profile screen. Run b90gfx-104111 (frames: the button between DISPLAY and RESET
DEFAULTS, the page titled GRAPHICS, back on the profile screen). Known: on the profile screen GRAPHICS shows
the help line of the button the cursor came from (the help handler sub_00193B40 has no entry for it).
The pause menu's Options (Instincts, same screen id P_EditProfile in cryingame.mgb: common.dat 0x4B8647,
30,236 bytes, entry 67, hash 0x14653EE6; buttons CONTROLLER OPTIONS and DISPLAY, count byte 0x54E3 = 6) gets
the same button: DISPLAY's record (0x57B4-0x58AE) copied with the GRAPHICS id, CONTROLLER OPTIONS' up link
(0x57B0) GRAPHICS, y 0xAF -> 0xD2; fci_disk.c's gfx_layouts table holds both screens. Its string table (in-game
strings, entry 395) has only TitleButton, ControlSetup and Video in P_EditProfile, so the unused
`B_CheatDebug` row of P_StartMenu (CHEATS (DEBUG); the element exists hidden in cryingame) moves there as
`B_ControllerScheme` GRAPHICS, the same 52 bytes, the rows between shifting up. The pause Options handler
sub_0014CEB0 (eax event, edi screen; B_ControlSetup -> [edi+0x64] vt+0x24(1)) gets the same action swap.
With both buttons in place the DISPLAY AND GRAPHICS row is left out of Controller Options. Runs b90gfx2-113242
(profile) and b90pause3-113425 and b90pause4-113549 (pause: OPTIONS with GRAPHICS, Controller Options ending at the
game's CONTROLLER SETUP button, the page titled GRAPHICS, back on OPTIONS). The pause GRAPHICS help line is
DISPLAY's (the copied record's).

Evolution has the same pieces: the profile strings keep the unused `B_ControllerScheme` row, B_Video reads
GAME OPTIONS. Its profile screen (cryprofile.mgb: common.dat 0x6E93BC, 75,837 bytes, entry 421, same hash;
count byte 0x1FF0 = 10) orders CHARACTER SETUP, CONTROLLER OPTIONS, GAME OPTIONS, MULTIPLAYER OPTIONS, RESET
DEFAULTS at x 0x30, y 0x7D + 0x23 steps, records in the CryCommon tail form; GRAPHICS goes after GAME OPTIONS
(record 0x242A-0x251B, MULTIPLAYER's up link 0x2610), and MULTIPLAYER and RESET DEFAULTS move down a step.
The pause Options (cryingame.mgb: 0x63061E, 34,349 bytes, entry 76; count byte 0x571B = 7) is Instincts'
layout at x 0x30 (Video 0x5B00-0x5C29, CONTROLLER OPTIONS' up link 0x5AFC). Handlers, all by the button-name
strings at 0x4BEB5C (B_ControlSetup) and 0x4BEB6C (B_Video): profile dispatcher sub_00123C3D (eax event,
screen on the stack; CONTROLLER OPTIONS -> sub_00122DBA), pause Options sub_000F9C7C (eax event, ecx screen;
[ecx+0x64] / +0x68 as Instincts), Controller Options' show sub_00118B88 (ecx screen; fill sub_00119C3D, then
labels; ret 4). Runs evogfx-114747 (profile with GRAPHICS, Controller Options without the row), evogfx2-114901
(the page from the profile, back), evopause4-115439 (pause OPTIONS with GRAPHICS, the page, back). The pause
menu needs one Start some 16 s into the first level; earlier presses are dropped during the opening.
