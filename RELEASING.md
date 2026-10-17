# Releasing

Releases are tagged from `main`. CI checks the tools, the patches and that the runtime compiles, but it
has no game disc, so the build and the game itself are checked by hand before every release.

## Versions

`0.x` until Story Mode and the map editor play through reliably, then 1.0. The middle number goes up
for new features (0.2.0), the last one for releases with only fixes (0.1.1).

## Steps

1. Everything for the release is merged and CI on `main` is green.
2. Clone `main` into an empty folder and build the game and the editor from the disc:

   ```
   python fci-recomp.py setup <disc> --editor --work <folder>\work --install <folder>
   ```

3. Run the regression suite on that install (`FCI_INSTALL=<folder>`, `FCI_REGRESS` set to the folder
   with the fixtures and references). Every scenario must pass:

   ```
   python -B tools/regress.py run
   ```

4. Play it briefly: the title screen, Story Mode from a save, the map editor and back.
5. In a pull request: rename `## [Unreleased]` in CHANGELOG.md to `## [x.y.z] - <date>` with a new empty
   `## [Unreleased]` above it, and set `VERSION` in fci-recomp.py. Merge it once CI passes.
6. Tag the merge commit on `main` and push the tag:

   ```
   git tag -a vx.y.z -m "fci-recomp x.y.z"
   git push origin vx.y.z
   ```

7. Create the release with that version's changelog section as the notes:

   ```
   gh release create vx.y.z --verify-tag --title "fci-recomp x.y.z" --notes-file notes.md
   ```

Release tags (`v*`) can't be moved or deleted once pushed. If a release is broken, fix it and release
the next version.

## Fixing an older release

Normally the fix goes into `main` and the next release. If `main` already has unfinished work that
shouldn't ship, branch `release/x.y` from the release's tag, fix it there through a pull request,
release x.y.(z+1) from that branch, then bring the fix into `main` too.
