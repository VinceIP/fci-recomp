# Story saves: "damaged and cannot be used"

Image: Instincts default.xbe. Found 2026-11-20 with driven runs on copies (frames and stderr) and the lifted
save writer. Evolution names its saves differently (fcs0/fcs1) and is not covered here.

## What a profile holds

A profile folder is `UDATA\<title>\<profile>`. Besides profile.sav, SaveMeta.xbx and the small files, it holds
exactly two `.fcs`: a level file, named as the level (`04_rain_forest.fcs`), and a checkpoint
(`Abandoned_Tunnels.fcs`, or ours, `Quick_<level>.fcs`). Each `.fcs` header names its level: name length
(u16) at +2, the name at +6. Checkpoints name their level too. Every data file ends in a 20-byte HMAC-SHA1 of
the bytes before it, keyed with 16 zero bytes (our kernel's XboxHDKey); sub_00055020 checks it.

## The sign-in check

The profile page (sub_00190C00) calls the profile "damaged" unless there are exactly two `.fcs`
(sub_0018EC20) and each one's level file `<level>.fcs` is present. Measured on copies:

| `.fcs` in the folder | Sign-in |
|---|---|
| 04_rain_forest, Abandoned_Tunnels | OK |
| 04_rain_forest, Quick_04_rain_forest | OK |
| 00_training, a copy of it named Quick_00_training | OK |
| 00_training alone | damaged |
| 00_training, Abandoned_Tunnels | damaged |
| 00_training, Quick_04_rain_forest | damaged |

## How a level start loses the level file

The save writer sub_000565D0 calls sub_00056320 to make room (esi = the profile folder, [esp+4] the game
object, [esp+8] the new file's name). If the new file already exists it is rewritten in place. Otherwise
sub_0018F6F0 lists the `.fcs` sorted by last-write time and the oldest is deleted, unless its name equals the
std::string at object+0x2A4 (the level loaded now); then the other one goes. The delete is a
delete-on-close disposition (sub_002A6250), not NtDeleteFile.

A checkpoint is written after its level file, so it ends newer (native run, Beach: 01_beach 06:52:46,
Beach_AfterCutscene 06:53:19). From the menu (Story Mode > Load a Map) no level is loaded and nothing is
protected, so the level file is deleted and the checkpoint stays without its pair. Our F5 quick save writes a
checkpoint at any moment, so it made this common; it is not needed for it (run b88menu3: a native checkpoint
newer than its level file, then Load a Map > Training, left {00_training, Abandoned_Tunnels}).

Story Resume (sub_000B3470 > sub_00057590 > sub_00054910) loads the newest `.fcs` and writes nothing.

## The runtime's fix (src/fci_saves.c)

- A prehook on sub_00056320: when the new name is not in the folder, the two `.fcs` are a level file and a
  checkpoint of that level, the level file is not the protected one and is not newer, the level file's
  last-write time is set to the checkpoint's plus one second. The game then deletes the checkpoint itself.
  Logged as `[FCI] saves: saving <name> with no level loaded: ...`.
- At startup, `fci_saves_repair` walks `UDATA\*\*`. A profile with a level file and a checkpoint whose level
  file is missing gets the checkpoint moved to `<save dir>\save-backups\<title>-<profile>-<time>\` (never
  deleted) and a copy of the level file, `Backup_<level>.fcs`, a minute older than the level file, in its
  place. The pair passes sign-in; Resume loads the newer level file. Logged as `[FCI] saves: profile ...`.

## Runs (Z:\llm\fci-recomp\regress\drive, stores saves\instincts\b88-*)

| Run | Store | Result |
|---|---|---|
| b88menu3 | b88-native-cpnewer | before the fix: Load a Map > Training left {00_training, Abandoned_Tunnels}, damaged |
| fix2-b88-native-cpnewer | b88-native-cpnewer | after: {00_training, 04_rain_forest}; the checkpoint went |
| fix2-b88-arch-quick | rainforest + Quick_04 | after: {00_training, 04_rain_forest} |
| sign-b88-fixed | b88-fixed | main menu, Story Mode, Resume into the level |
| sign-b88-before | the damaged folder from the report | repaired at startup; main menu, Resume into Training |

Open: starting a level with a posted `StartLevel` from inside a level crashes at sub_002C1530+0x24A (fault
guestva D9247208), with or without F5. Only the driver posts that; the menu path does not.
