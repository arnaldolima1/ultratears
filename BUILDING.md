# Building UltraTears from the source

This page is for developers and people testing source changes. Players who only want to build and play should follow
the **builder package from Releases** instructions in [README.md](README.md) or [README.ru.md](README.ru.md); that route
requires no Git, Python or terminal commands.

A source build has three locations:

| Location | Example | Purpose |
|---|---|---|
| Source checkout | `D:\src\ultratears` | Git-tracked source code and, by default, the original `game\exefs` and `game\romfs` |
| Output folder | `D:\src\ultratears\dist` | Build tools, intermediate files, converted game data and the final `UltraTears.exe` |
| Tool download cache | `D:\src\ultratears\dev-local\downloads` | The downloaded builder archive, reused when making another output folder |

`python tools/setup.py` prepares the output folder **and immediately performs the complete game build**. It is not a
tools-only preparation command, so the ExeFS and RomFS must already be available when it is run.

## What you need on your PC

| Tool | Version used | Notes |
|---|---|---|
| Windows 10/11 x64 | | |
| Python | Any maintained Python 3 to start `tools/setup.py`; its local Python 3.14 is downloaded automatically | The build uses the downloaded Python with `lz4`, `capstone` and `unicorn` |
| Build tools | downloaded automatically | Python 3.14, the trimmed MSYS2 CLANG64 toolchain and `ultrs-shaders.exe` are taken from the latest builder release |
| git | | for the clone |

The first run needs network access and downloads the latest builder archive (currently about 150 MiB) once into
`dev-local\downloads`. Only its tools and licenses are installed in `<output>\deps`; no game files are downloaded.
Later builds reuse both the cache and the installed tools.

## Build

```
git clone <URL of this repository> UltraTears
cd UltraTears
python tools/setup.py
```

The command copies tracked source files (including uncommitted edits) to `dist\`, installs missing tools and performs
every game-build step. It reads the original dump from `game\` in the checkout without moving or modifying it. The
result is `dist\UltraTears.exe`, supported by the converted files in `dist\data`.

Path selection is explicit and predictable:

| Command | Output | Dump read from |
|---|---|---|
| `python tools/setup.py` | `<checkout>\dist` | `<output>\game` if populated, otherwise `<checkout>\game`, otherwise the last `--dump` |
| `python tools/setup.py --output D:\Games\UltraTears` | `D:\Games\UltraTears` | `<output>\game` if populated, otherwise `<checkout>\game`, otherwise the last `--dump` |
| `python tools/setup.py --dump D:\Dumps\TOTK` | `<checkout>\dist` | `D:\Dumps\TOTK` |
| `python tools/setup.py --output D:\Games\UltraTears --dump D:\Dumps\TOTK` | `D:\Games\UltraTears` | `D:\Dumps\TOTK` |

`--cpus N` limits the logical processors used (default: all, at low priority so the PC stays usable). `--only <step>`
runs one step after the mandatory check. Repeating the same command synchronizes changed source files and resumes the
incremental build. The output folder's `data`, `build`, `deps`, `userdata` and diagnostics are preserved.

The steps, what they write and how long they take on an 8-core PC:

| Step | Writes | Time | Runs again when |
|---|---|---|---|
| `check` | nothing: checks the tools, the SHA-256 of `exefs\main`, `subsdk0`, `sdk` (1.4.3) and refuses encrypted packages | seconds | always |
| `romfs` | `data\romfs\romfs.bin` + index: the RomFS folder in one file, read with 16 threads; its content hash is checked against 1.4.3 | ~4 min | the image is missing |
| `lift` | `data\exefs` (a copy of the code modules), `data\generated` (the game's code as C, ~0.9 GB) | ~2 min | the lifter changed |
| `compile` | `UltraTears.exe` (~0.4 GB) and `build\` (objects ~1.8 GB, debug symbols) | ~15 min (seconds when little changed) | sources changed (ninja) |
| `shaders` | `data\shaders` (SPIR-V of the 28,769 shaders) | ~5 min | the finder or the translator changed; new shaders only otherwise |
| `textures` | `data\textures` (BC7 / BC1 of every ASTC texture, ~7.4 GB) | ~7 min | textures not yet in the cache |
| `finish` | `ultratears.cfg` | seconds | always |

`data\`, `build\`, `userdata\`, `ultratears.cfg` and `UltraTears.exe` in the output folder hold material made from your
copy of the game and must never be shared. After a complete build, runtime paths in `ultratears.cfg` point to the
output's own `data`; the original dump is no longer read when `UltraTears.exe` starts.

To build directly into a separate game folder:

```
python tools/setup.py --output D:\Games\UltraTears
```

## Where the game looks for its data

`UltraTears.exe` reads `ultratears.cfg` next to it: the ExeFS (`exefs=`) and the `TOTK_*` variables for the RomFS,
shaders, texture cache and saves. Without that file it uses the development layout (`work\` of the developer's
machine). The RomFS may also be a plain folder (`TOTK_ROMFS=<folder>`): it is indexed when the game
starts, but loading reads many small files, which is slower; the packed image is the default.

## Troubleshooting

- A tool download fails: check the connection to GitHub and run the same command again. A partial download is not used.
- A tool under `<output>\deps` is damaged: remove that output folder's `deps` and run setup again; the cached archive
  is reused.
- `exefs\main is not the one of Tears of the Kingdom 1.4.3`: extract the ExeFS again with update 1.4.3 applied.
- `the RomFS is not the one of ... 1.4.3`: extract the RomFS again with the update applied, without mods.
- `encrypted packages are not accepted`: give the extracted folders, not an `.nsp` / `.xci` / `.nca`.
- The game starts with a black window or a Vulkan error: update the graphics driver (Vulkan 1.3 with
  `VK_EXT_shader_object` is needed).

## Making the builder package (maintainers)

```
python tools/build_xlate.py
git tag v<version>
python tools/setup.py
python tools/make_package.py
python tools/public_tree.py --publish
```

The version has one source, the tag `v<x.y.z>` on the release commit (`tools/version.py`): the package, the public
commit and the exe take it from there. The exe writes it as the first line of its log and shows it in the file's
properties; between releases it reads like `0.1.0-3-gabc1234` (`git describe`).

It needs a complete build in the output folder (`dist\` by default; built elsewhere with `tools/setup.py --output
<folder>`, pass `--home <folder>`) and a clean working tree,
and writes `dev-local\dist\UltraTears-builder-<version>-windows-x86_64.zip`: the public tree of HEAD, the part of MSYS2 CLANG64 the build
reads (measured from ninja's dependency records and the linker's trace), Python 3.14 with the packages above,
`ultrs-shaders.exe`, `setup.bat`, and the licenses of all of it.
