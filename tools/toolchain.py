"""Where the build's tools are: the builder package's deps/ first, then the developer's own installs.

Windows: CLANG64, a folder with clang, clang++, lld, ninja and the static libraries the port links (SDL3, opus, vpx,
zstd, iconv, libc++): deps/clang64/bin in the builder package (a trimmed MSYS2 CLANG64), else ULTRS_CLANG64, else
C:/msys64/clang64/bin.
macOS (fork): Apple's clang (Xcode Command Line Tools) and Homebrew's ninja, SDL3, opus, libvpx, zstd and molten-vk
(HOMEBREW; tools/setup.py installs what is missing). The build's Python is deps/python, a venv.
XLATE: the shader translator ultrs-shaders (tools/build_xlate.py).
"""
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
MAC = sys.platform == 'darwin'
EXE = '' if MAC else '.exe'
HOMEBREW = pathlib.Path('/opt/homebrew' if pathlib.Path('/opt/homebrew/bin/brew').is_file() else '/usr/local')
BREW_FORMULAS = ('ninja', 'sdl3', 'opus', 'libvpx', 'zstd', 'molten-vk', 'vulkan-loader', 'vulkan-headers',
                 'vulkan-extensionlayer')   # (the last: VK_EXT_shader_object over MoltenVK)


def _clang64():
    for c in (ROOT / 'deps/clang64/bin', os.environ.get('ULTRS_CLANG64'), 'C:/msys64/clang64/bin'):
        if c and (pathlib.Path(c) / 'clang.exe').is_file():
            return pathlib.Path(c)
    return None


if MAC:
    CLANG64 = None
    CC, CXX = shutil.which('clang') or '/usr/bin/clang', shutil.which('clang++') or '/usr/bin/clang++'
    NINJA = str(HOMEBREW / 'bin/ninja')
else:
    CLANG64 = _clang64()
    CC = CXX = NINJA = None
    if CLANG64:
        CC, CXX, NINJA = str(CLANG64 / 'clang.exe'), str(CLANG64 / 'clang++.exe'), str(CLANG64 / 'ninja.exe')
XLATE = next((p for p in (ROOT / f'deps/shaders/ultrs-shaders{EXE}', ROOT / f'dev-local/build/shaders/ultrs-shaders{EXE}')
              if p.is_file()), None)
# The build's Python: the builder package's (Windows) or the venv tools/setup.py makes (macOS)
PYTHON = ROOT / ('deps/python/bin/python3' if MAC else 'deps/python/python.exe')


def env():
    """The environment for the build's commands: the compiler first on PATH."""
    if MAC:
        if not (CC and pathlib.Path(NINJA).is_file()):
            raise SystemExit('clang or ninja not found: install the Xcode Command Line Tools (xcode-select --install) '
                             'and Homebrew\'s ninja')
        return dict(os.environ, PATH=os.pathsep.join([str(HOMEBREW / 'bin'), os.environ.get('PATH', '')]))
    if CLANG64 is None:
        raise SystemExit('clang not found: unpack the builder package (deps/clang64) or install MSYS2 CLANG64 '
                         '(see BUILDING.md), or set ULTRS_CLANG64 to its bin folder')
    return dict(os.environ, PATH=str(CLANG64) + os.pathsep + os.environ.get('PATH', ''))


def popen_limited(cmd, env=None, cpus=8, **kw):
    """Start a heavy command at low priority so the computer stays usable; its children inherit it. Windows: also on
    the first `cpus` logical CPUs only (affinity, set while it is suspended). macOS has no affinity: the callers
    limit the parallelism (ninja -j, worker counts) and the priority is background (nice 10)."""
    if MAC:
        return subprocess.Popen(cmd, env=env, preexec_fn=lambda: os.nice(10), **kw)
    import ctypes
    CREATE_SUSPENDED, BELOW_NORMAL_PRIORITY_CLASS = 0x4, 0x4000
    p = subprocess.Popen(cmd, env=env, creationflags=CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, **kw)
    if not ctypes.windll.kernel32.SetProcessAffinityMask(int(p._handle), ctypes.c_size_t((1 << cpus) - 1)):
        p.kill()
        raise SystemExit('SetProcessAffinityMask failed')
    ctypes.windll.ntdll.NtResumeProcess(int(p._handle))
    return p


def run_limited(cmd, env=None, cpus=8):
    return popen_limited(cmd, env, cpus).wait()


def brew_missing():
    """Homebrew formulas the macOS build needs that are not installed."""
    if not MAC:
        return []
    brew = HOMEBREW / 'bin/brew'
    if not brew.is_file():
        return list(BREW_FORMULAS)
    have = set(subprocess.run([str(brew), 'list', '--formula', '-1'], capture_output=True, text=True).stdout.split())
    return [f for f in BREW_FORMULAS if f not in have]
