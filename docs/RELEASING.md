# Releasing a new version

```
.\scripts\release.ps1 0.2.0 -DryRun                    # rehearse: publishes nothing
.\scripts\release.ps1 0.2.0 -Notes "What changed"      # the real thing
```

Write the notes yourself (`-Notes`): they are **public**. Without them the notes are the commit
titles since the last release.

## Two repositories

- A **private working repository** (`origin`): day-to-day commits and their full history.
- The **public repository** `CainanNevils/cutline`: the GPL source, **one commit per release**
  (the release's source, minus the internal files listed in `scripts/public-exclude.txt`), and the
  Releases page with the installer zips.

## What the command does

1. Refuses to run unless you're on `main`, everything is committed and `main` matches `origin`.
2. Sets the version in `CMakeLists.txt` (`project(cutline VERSION ...)`): it goes into the exe's
   file properties, the About box, the log and the zip name.
3. Builds and runs every test suite (`scripts\test.ps1`). **If anything fails it stops and
   changes nothing.**
4. Packages `dist\Cutline-<version>-win64.zip` and a `.sha256` checksum (`scripts\package.ps1`),
   checks the exe reports the new version and **scans the package for personal information**
   (`scripts\scan-package.ps1`).
5. Builds the **public source snapshot** and scans it the same way, plus for the words in
   `scripts\release-banned-words.txt` (which, like the other excluded files, is never published).
6. Downloads the source code of the exact FFmpeg version inside the package: the GPL requires
   publishing it next to the binaries.
7. Commits "Release v<version>", tags `v<version>` and pushes both to the private repository.
8. Pushes the snapshot to the public repository as a new commit on `main` with the tag
   `v<version>`, and creates the release there with the zip, its checksum and the FFmpeg source.

The very first release may use the current version (0.1.0); after that versions only go up.

`-DryRun` does steps 2-6 into `dist\dryrun\`, undoes the version edit and stops: nothing is
committed, tagged, pushed or uploaded. Its public snapshot is
`dist\dryrun\Cutline-<version>-public-source.zip`: that is exactly what would become public.

Needs: the build tools (as for `scripts\build.ps1`), the generated test media
(`scripts\gen-testmedia.ps1`, used by the tests) and the GitHub CLI logged in (`gh auth status`).

## Licensing (what each release must carry)

Cutline is GPL v3 or later (`LICENSE`) because it ships a GPL v3 FFmpeg build. Every release has:
the installer zip (with `LICENSE` and `THIRD_PARTY_NOTICES.txt` inside), the Cutline source at the
release tag, and the FFmpeg source archive. Qt is LGPL v3, dynamically linked; its source is
linked from `THIRD_PARTY_NOTICES.txt`. Don't add a dependency without checking its license fits
the GPL v3.

## Installing (what users do)

1. Download `Cutline-<version>-win64.zip` from the Releases page, unzip it.
2. Double-click `Install.cmd`. Windows may say "Windows protected your PC" (the app isn't
   code-signed): **More info > Run anyway**.
3. Cutline opens, with Desktop and Start Menu shortcuts and an entry in Settings > Apps.

Updating is the same steps with the new zip: `Install.cmd` replaces the old program files and
keeps projects, settings and the media cache (`%LOCALAPPDATA%\Cutline`). It asks you to close
Cutline first if it is running. Uninstall: Settings > Apps > Cutline > Uninstall.

## If something goes wrong

- **A release went out broken:** release a fixed, higher version (e.g. 0.2.1).
- **A push or upload failed partway:** the script prints the exact command to retry.
- **Try a release before anyone sees it:** run `Install.cmd` from the dry-run package.
- Logs: `%LOCALAPPDATA%\Cutline\cutline.log`.
