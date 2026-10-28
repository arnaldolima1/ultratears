---
name: release
description: Release a new UltraTears Launcher version with real release notes. Use when the user asks to release, publish, ship or "subir" a launcher version (patch, minor, major or X.Y.Z), or types /release.
---

# Release the launcher

A launcher release is a tag `launcher-v<version>`: `launcher/release.py` bumps the version, commits, tags and pushes,
and `.github/workflows/launcher-release.yml` builds the installers and publishes the release with the notes. The notes
show on the GitHub release and on the update card of every installed launcher, so they are written for players.

## Steps

1. Check the tree: `git status --porcelain --untracked-files=no` must be empty (commit first, asking the user) and
   the branch should be `develop` (release.py fast-forwards `main` to it).
2. Next version: the argument (`patch` by default, or `minor`, `major`, `X.Y.Z`) applied to `version` in
   `launcher/src-tauri/tauri.conf.json`.
3. What changed: `git describe --tags --match "launcher-v*" --abbrev=0`, then
   `git log --no-merges --format="%h %s%n%b" <that tag>..HEAD`. Read the commit bodies, and the diff (`--stat`, then the
   files that matter) where a subject is unclear. Leave out pure version bumps (`launcher X.Y.Z`).
4. Write the notes to a scratch file (not in the repo), Markdown, in this shape:

   ```markdown
   ## Novidades
   - <what a player notices, one line each: new features first, then fixes>

   ## What's new
   - <the same points in English>
   ```

   Rules: Brazilian Portuguese first, then English; 3 to 8 bullets per language; say what the player sees or can now do
   ("O painel F1 abre pela esquerda com abas", not "overlay.cpp rewritten"); group many small commits into one
   bullet; no commit hashes, file names or internal jargon; mention the game (not only the launcher) when the commits
   change the game, since installed launchers then offer the game update too. Never invent a change the commits do
   not show.
5. Show the notes and the version to the user and wait for their go-ahead (a release is public and starts a build).
6. Run `python launcher/release.py <version or patch|minor|major> --notes <scratch file>` from the repository root.
   It writes `launcher/release-notes/<version>.md` into the release commit.
7. Watch the build: `gh run list -R arnaldolima1/ultratears -L 1`, then `gh run watch <id> -R arnaldolima1/ultratears
   --exit-status` in the background. When it ends, check `gh release view launcher-v<version> -R arnaldolima1/ultratears`
   lists `latest.json` and the installers, and tell the user.

`--replace` releases the current version again (deletes its release and tag first): only when the user asks for it.
