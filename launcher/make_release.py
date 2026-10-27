"""Make the launcher release of this fork: dev-local/dist/UltraTears-Launcher-<version>-windows-x86_64.zip.

usage: python launcher/make_release.py --version 0.1.0-launcher.1 [--home ..\\UltraTears]
The zip is the builder package (tools/make_package.py) with the launcher at its root:
  - the source: what git tracks at HEAD, without launcher/, docs/ and the artwork in assets/ (only assets/icon.ico,
    which the build links into the exe);
  - deps/ (python, clang64, shaders), licenses/ and THIRD-PARTY-NOTICES.md taken from --home, a game folder built from
    an official builder package (its tools are what make_package.py measured);
  - setup.bat, game/PUT-YOUR-GAME-HERE.txt and VERSION.txt as make_package.py writes them;
  - UltraTears Launcher.exe (npx tauri build --no-bundle in launcher/ first).
Nothing made from the game goes in: data/, build/, userdata/, the dump and the exe are never read. Refuses a dirty
working tree, so the zip is the commit it names.
"""
import argparse
import pathlib
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import make_package  # noqa: E402
import version  # noqa: E402

SKIP_SOURCE = ('launcher/', 'docs/', 'assets/', '.github/')
KEEP_SOURCE = {'assets/icon.ico'}
DEPS = ['deps/python', 'deps/clang64', 'deps/shaders', 'licenses']
NEVER = ('data/', 'build/', 'userdata/', 'game/exefs', 'game/romfs', 'diag/', 'UltraTears.exe', 'ultratears.cfg')


def git(*args):
    return subprocess.run(['git', '-C', str(ROOT)] + list(args), check=True, capture_output=True, text=True).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', required=True)
    ap.add_argument('--home', type=pathlib.Path, default=ROOT.parent / 'UltraTears')
    a = ap.parse_args()
    if git('status', '--porcelain', '--untracked-files=no').strip():
        sys.exit('the working tree has uncommitted changes: commit first')
    commit = git('rev-parse', '--short', 'HEAD').strip()
    launcher = ROOT / 'launcher/src-tauri/target/release/ultratears-launcher.exe'
    if not launcher.is_file():
        sys.exit(f'{launcher} is missing: run npx tauri build --no-bundle in launcher/')
    for d in DEPS + ['THIRD-PARTY-NOTICES.md']:
        if not (a.home / d).exists():
            sys.exit(f'{a.home / d} is missing: --home must be a game folder made from a builder package')

    name = f'UltraTears-Launcher-{a.version}-windows-x86_64'
    out = ROOT / 'dev-local/dist' / f'{name}.zip'
    out.parent.mkdir(parents=True, exist_ok=True)
    top = 'UltraTears/'
    entries = 0

    def add(zf, arc, path=None, data=None):
        nonlocal entries
        assert not arc.startswith(NEVER), arc
        if data is not None:
            zf.writestr(top + arc, data)
        else:
            zf.write(path, top + arc)
        entries += 1

    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for f in git('ls-files', '-z').split('\0'):
            if f and (f in KEEP_SOURCE or not f.startswith(SKIP_SOURCE)) and (ROOT / f).is_file():
                add(zf, f, ROOT / f)
        for d in DEPS:
            for p in sorted((a.home / d).rglob('*')):
                if p.is_file() and '__pycache__' not in p.parts:
                    add(zf, p.relative_to(a.home).as_posix(), p)
        add(zf, 'THIRD-PARTY-NOTICES.md', a.home / 'THIRD-PARTY-NOTICES.md')
        add(zf, 'setup.bat', data=make_package.SETUP_BAT.replace('\n', '\r\n'))
        add(zf, 'game/PUT-YOUR-GAME-HERE.txt', data=make_package.GAME_TXT.replace('\n', '\r\n'))
        add(zf, 'VERSION.txt', data=version.version_txt(a.version, commit))
        add(zf, 'UltraTears Launcher.exe', launcher)
    print(f'{out}: {entries} files, {out.stat().st_size / 2**20:.0f} MiB (commit {commit})')


if __name__ == '__main__':
    main()
