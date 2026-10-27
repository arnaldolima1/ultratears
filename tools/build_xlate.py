"""Build the shader translator ultrs-shaders (MacNativeGames' totk_xlate service around the Ryujinx 1.3.2 shader
translator in third_party/maxwell_shader, both MIT) as one self-contained Windows program: build/shaders/ultrs-shaders.exe, which needs no .NET
installed to run (dev-local/build/shaders; tools/setup.py and tools/make_package.py take it from there). It turns the game's Switch GPU shaders (Maxwell machine code) into SPIR-V for Vulkan.

usage: python tools/build_xlate.py [--dotnet dotnet]
Sources: third_party/shader_translator (the service; its patches/ are already applied to the copy in third_party/maxwell_shader,
kept as the record of what differs from Ryujinx 1.3.2). Needs the .NET 10 SDK and, the first time, network access for
the NuGet packages; everything it fetches stays inside the project (dev-local/nuget, dev-local/dotnet-home).
The builder package carries the result, so players never run this.
"""
import argparse
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dotnet', default='dotnet')
    ap.add_argument('--rid', default='osx-arm64' if sys.platform == 'darwin' else 'win-x64',
                    help='.NET runtime to publish for (default: this computer\'s)')
    a = ap.parse_args()
    out = ROOT / 'dev-local/build/shaders'   # a development tool's build: no game data, not in git
    env = dict(os.environ, NUGET_PACKAGES=str(ROOT / 'dev-local/nuget'), DOTNET_CLI_HOME=str(ROOT / 'dev-local/dotnet-home'),
               DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1', DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1')
    proj = ROOT / 'third_party/shader_translator/ultrs-shaders.csproj'
    rc = subprocess.run([a.dotnet, 'publish', str(proj), '-c', 'Release', '-r', a.rid, '--self-contained', 'true',
                         '-p:PublishSingleFile=true', '-p:InvariantGlobalization=true',
                         '-p:IncludeNativeLibrariesForSelfExtract=true',
                         f'-p:RyuSrc={ROOT / "third_party/maxwell_shader"}',
                         '-o', str(out), '-nologo', '-v:q', '-maxcpucount:4'], env=env).returncode
    if rc:
        sys.exit(rc)
    exe = out / ('ultrs-shaders.exe' if a.rid.startswith('win') else 'ultrs-shaders')
    for f in out.iterdir():   # one program; the debug symbols are not needed
        if f.suffix == '.pdb':
            f.unlink()
    print(f'built {exe} ({exe.stat().st_size / 1e6:.0f} MB)')


if __name__ == '__main__':
    main()
