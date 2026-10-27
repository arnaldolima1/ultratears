"""Lift the three TOTK code modules to C (tools/lifter/gen.py), in parallel.

usage: python tools/gen_game.py <exefs dir> <output dir>
Bases must match src/runtime/rt.h (MAIN_BASE, SUBSDK0_BASE, SDK_BASE).
"""
import pathlib
import platform
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
MODULES = {'main': 0x0800000000, 'subsdk0': 0x0804000000, 'sdk': 0x0805000000}
# FP/SIMD: native AArch64 inline asm on an ARM64 host (macOS on Apple silicon: bit-exact, MacNativeGames' way), the
# portable C lowering (runtime/simd.h) on x86-64
SIMD = 'asm' if platform.machine().lower() in ('arm64', 'aarch64') else 'c'


def main():
    exefs, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    t0 = time.time()
    procs = []
    for name, base in MODULES.items():
        (out / name).mkdir(parents=True, exist_ok=True)
        log = open(out / f'{name}.log', 'w')
        procs.append((name, log, subprocess.Popen(
            [sys.executable, str(ROOT / 'tools/lifter/gen.py'), name, str(exefs / name), hex(base), str(out / name),
             '--simd', SIMD], stdout=log, stderr=subprocess.STDOUT)))
    failed = False
    for name, log, p in procs:
        code = p.wait()
        log.close()
        summary = [l for l in (out / f'{name}.log').read_text().splitlines() if 'progress' not in l]
        print(f'{name}: exit {code}; ' + ' | '.join(summary[-2:]))
        failed |= code != 0
    print(f'{time.time() - t0:.0f}s')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
