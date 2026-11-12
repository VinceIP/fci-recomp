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
