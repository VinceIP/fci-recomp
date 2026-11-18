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
stderr). The runtime's `fci_settings_menu.c` adds a RECOMP SETTINGS row to Controller Options (Options →
profile → Controller Options, and the pause menu's Options); A on it fills the same list with the fci.ini
settings.

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

Gates: the fill adds our entry row after the game's; A on the entry clears the list and adds one COption
per setting (the game's handler is skipped); after the game's own toggle, rows rebuild and set-index, a
changed index is applied live (graphics options and engine variables) and written to fci.ini, or saved
for the next start. The title element `B_TitleButton` reads RECOMP SETTINGS while ours are shown, and is
put back after the value popup closes. Leaving with B returns to the profile screen; the next visit shows
the game's own options again.

Runs: Instincts q75-set1-090436 (pause menu) and q75main1-084226 (main menu); Evolution q75-evoset-085704 and -090042
(the second read the first's fci.ini: anti-aliasing OFF, toggled back ON). Known: on opening, the help
box keeps the entry's text until the cursor moves (the help call runs with row 0; something later
restores it, not found).
