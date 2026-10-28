"""Release the launcher: bump its version everywhere, commit, tag launcher-v<version> and push.

usage: python launcher/release.py 0.2.1 | patch | minor | major [--notes FILE] [--replace] [--no-push]
  - writes the version into tauri.conf.json, Cargo.toml, Cargo.lock, package.json and package-lock.json;
  - commits "launcher <version>" and tags launcher-v<version>;
  - pushes the branch, fast-forwards main to it (the game updates read main) and pushes the tag, which starts
    .github/workflows/launcher-release.yml (installers + latest.json, signed with the TAURI_SIGNING_PRIVATE_KEY secret).
--replace releases the current version again: deletes its GitHub release and tag first (gh), no bump.
--notes FILE: the release notes (Markdown, Portuguese then English: what players see on the release and on the
  launcher's update card), kept as launcher/release-notes/<version>.md; without it, the commit subjects since the last
  launcher tag (the /release command in .claude/skills writes proper ones).
--no-push stops after the local commit and tag.
Refuses a dirty working tree. Warns when ~/.tauri/ultratears-launcher.key.pub is not the pubkey in tauri.conf.json.
"""
import argparse
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
LAUNCHER = ROOT / 'launcher'
TAURI_CONF = LAUNCHER / 'src-tauri/tauri.conf.json'
CARGO_TOML = LAUNCHER / 'src-tauri/Cargo.toml'
CARGO_LOCK = LAUNCHER / 'src-tauri/Cargo.lock'
PACKAGE = LAUNCHER / 'package.json'
PACKAGE_LOCK = LAUNCHER / 'package-lock.json'
NOTES = LAUNCHER / 'release-notes'
PUBKEY = pathlib.Path.home() / '.tauri/ultratears-launcher.key.pub'
REPO = 'arnaldolima1/ultratears'
SEMVER = re.compile(r'^\d+\.\d+\.\d+$')


def run(*args, check=True):
    print('>', ' '.join(args))
    return subprocess.run(args, cwd=ROOT, check=check, capture_output=True, text=True).stdout.strip()


def sub_once(path, pattern, repl):
    raw = path.read_bytes().decode('utf-8')
    eol = '\r\n' if '\r\n' in raw else '\n'   # keep the checkout's line endings (core.autocrlf)
    new, n = re.subn(pattern, repl, raw.replace('\r\n', '\n'), count=1, flags=re.M)
    if n != 1:
        sys.exit(f'{path.name}: version not found')
    path.write_bytes(new.replace('\n', eol).encode('utf-8'))


def write_version(v):
    sub_once(TAURI_CONF, r'^(  "version": )"[^"]+"', rf'\g<1>"{v}"')
    sub_once(PACKAGE, r'^(  "version": )"[^"]+"', rf'\g<1>"{v}"')
    sub_once(PACKAGE_LOCK, r'^(  "version": )"[^"]+"', rf'\g<1>"{v}"')
    sub_once(PACKAGE_LOCK, r'^(    "": \{\n      "name": "ultratears-launcher",\n      "version": )"[^"]+"',
             rf'\g<1>"{v}"')
    sub_once(CARGO_TOML, r'^(\[package\]\nname = "ultratears-launcher"\nversion = )"[^"]+"', rf'\g<1>"{v}"')
    sub_once(CARGO_LOCK, r'^(name = "ultratears-launcher"\nversion = )"[^"]+"', rf'\g<1>"{v}"')


def write_notes(v, src):
    """launcher/release-notes/<v>.md: the given file, else the commit subjects since the last launcher tag."""
    NOTES.mkdir(exist_ok=True)
    if src:
        text = pathlib.Path(src).read_text(encoding='utf-8').strip() + '\n'
    else:
        last = run('git', 'describe', '--tags', '--match', 'launcher-v*', '--abbrev=0', 'HEAD', check=False)
        log = run('git', 'log', '--no-merges', '--format=%s', f'{last}..HEAD' if last else 'HEAD~20..HEAD')
        subjects = [s for s in log.splitlines() if s and not re.match(r'^launcher \d+\.\d+\.\d+$', s)]
        text = ''.join(f'- {s}\n' for s in subjects) or '- Correções e melhorias. / Fixes and improvements.\n'
    path = NOTES / f'{v}.md'
    path.write_text(text, encoding='utf-8', newline='\n')
    run('git', 'add', str(path))


def bump(cur, how):
    if SEMVER.match(how):
        return how
    major, minor, patch = map(int, cur.split('.'))
    if how == 'major':
        return f'{major + 1}.0.0'
    if how == 'minor':
        return f'{major}.{minor + 1}.0'
    if how == 'patch':
        return f'{major}.{minor}.{patch + 1}'
    sys.exit(f'{how}: give X.Y.Z, patch, minor or major')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('version', nargs='?', default='patch')
    ap.add_argument('--notes')
    ap.add_argument('--replace', action='store_true')
    ap.add_argument('--no-push', action='store_true')
    a = ap.parse_args()

    if run('git', 'status', '--porcelain', '--untracked-files=no'):
        sys.exit('the working tree has uncommitted changes: commit first')
    conf = json.loads(TAURI_CONF.read_text(encoding='utf-8'))
    cur = conf['version']
    if PUBKEY.is_file() and PUBKEY.read_text().strip() != conf['plugins']['updater']['pubkey']:
        print(f'warning: {PUBKEY} is not the pubkey in tauri.conf.json; the release would not verify')

    branch = run('git', 'rev-parse', '--abbrev-ref', 'HEAD')
    if a.replace:
        v = cur
        tag = f'launcher-v{v}'
        run('gh', 'release', 'delete', tag, '-R', REPO, '--yes', '--cleanup-tag', check=False)
        run('git', 'tag', '-d', tag, check=False)
        write_version(v)   # brings package-lock and friends in line if they drifted
        write_notes(v, a.notes)
        if run('git', 'status', '--porcelain', '--untracked-files=no'):
            run('git', 'commit', '-am', f'launcher {v}')
    else:
        v = bump(cur, a.version)
        if tuple(map(int, v.split('.'))) <= tuple(map(int, cur.split('.'))):
            sys.exit(f'{v} is not newer than {cur} (use --replace to release {cur} again)')
        tag = f'launcher-v{v}'
        write_version(v)
        write_notes(v, a.notes)
        run('git', 'commit', '-am', f'launcher {v}')
    run('git', 'tag', tag)
    print(f'{cur} -> {v}, tag {tag} on {branch}')

    if a.no_push:
        print(f'not pushed; later: git push origin {branch} {branch}:main {tag}')
        return
    run('git', 'push', 'origin', branch)
    if branch != 'main':
        run('git', 'push', 'origin', f'{branch}:main')   # fast-forward only; fails if main has its own commits
    run('git', 'push', 'origin', tag)
    print(f'workflow started: https://github.com/{REPO}/actions')


if __name__ == '__main__':
    main()
