"""Translate the exported shaders with totk_xlate --serve (Ryujinx), in parallel on at most 8 logical CPUs.

usage: python tools/run_ryujinx_xlate.py data/shaders [--workers 8]
Writes <dir>/ryujinx/<key>.spv and <dir>/ryujinx-results.tsv (key, ok/fail, seconds, spv bytes, message).
"""
import argparse
import json
import os
import pathlib
import subprocess
import sys
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
from toolchain import XLATE, popen_limited  # noqa: E402


def start_pinned(cmd, cpus, env):
    return popen_limited(cmd, env, cpus, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dir', type=pathlib.Path)
    ap.add_argument('--workers', type=int, default=8)
    ap.add_argument('--new-only', action='store_true', help='translate only keys without a .spv')
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args()
    lines = (a.dir / 'jobs.jsonl').read_text().splitlines()
    (a.dir / 'ryujinx').mkdir(exist_ok=True)
    if a.new_only:   # only shaders without a translation yet (the comparison's outputs stay)
        # (one listing, not a lookup per shader: exFAT has no directory index, ~28,000 of them took minutes cold)
        have = set(os.listdir(a.dir / 'ryujinx'))
        lines = [l for l in lines if json.loads(l)['job']['key'] + '.spv' not in have]
    env = dict(os.environ, DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1',
               DOTNET_CLI_HOME=str(ROOT / 'third_party/dotnet-home'))
    if XLATE is None:
        sys.exit('the shader translator ultrs-shaders is missing: python tools/build_xlate.py')
    cmd = [str(XLATE)]   # tools/build_xlate.py, or the builder package's deps/shaders
    results, lock = [], threading.Lock()
    shards = [lines[i::a.workers] for i in range(a.workers)]
    t0 = time.time()

    def worker(shard):
        p = start_pinned(cmd + ['--serve'], a.workers, env)
        for line in shard:
            key = json.loads(line)['job']['key']
            s = time.perf_counter()
            p.stdin.write(line + '\n'); p.stdin.flush()
            reply = p.stdout.readline().strip()
            dt = time.perf_counter() - s
            spv = a.dir / 'ryujinx' / f'{key}.spv'
            ok = reply.startswith('ok ')
            size = spv.stat().st_size if ok and spv.exists() else 0
            msg = '' if ok else (reply[len('fail ' + key) + 1:] if reply.startswith('fail') else reply or 'no reply')
            with lock:
                results.append((key, 'ok' if ok else 'fail', dt, size, msg.replace('\t', ' ')[:300]))
                if len(results) % 2000 == 0 and not a.quiet:
                    print(f'{len(results)}/{len(lines)} {time.time() - t0:.0f}s', flush=True)
        p.stdin.close(); p.wait()

    threads = [threading.Thread(target=worker, args=(s,)) for s in shards]
    for t in threads: t.start()
    for t in threads: t.join()
    with (a.dir / ('ryujinx-results-new.tsv' if a.new_only else 'ryujinx-results.tsv')).open('w') as f:   # --new-only keeps the full table
        f.write('key\tresult\tseconds\tspv_bytes\tmessage\n')
        for r in sorted(results):
            f.write(f'{r[0]}\t{r[1]}\t{r[2]:.4f}\t{r[3]}\t{r[4]}\n')
    ok = sum(r[1] == 'ok' for r in results)
    print(f'ryujinx: {ok}/{len(results)} ok, {time.time() - t0:.0f}s wall, {sum(r[2] for r in results):.0f}s busy')


if __name__ == '__main__':
    main()
