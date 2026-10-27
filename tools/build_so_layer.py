"""Build Khronos' shader object layer (Vulkan-ExtensionLayer, Apache-2.0) for macOS with the UltraTears patch
(third_party/patches/vulkan-extensionlayer-shader-object-restart.patch: every pipeline it makes asks for primitive
restart, which Metal cannot turn off; MoltenVK fails the pipeline otherwise). Homebrew's build of the layer lacks it.

usage: python3 tools/build_so_layer.py
Writes dev-local/build/vulkan/libVkLayer_khronos_shader_object.dylib (tools/setup.py installs it in deps/vulkan).
Needs git, cmake, ninja and Homebrew's vulkan-headers, vulkan-utility-libraries and spirv-headers.
"""
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TAG = 'vulkan-sdk-1.4.363.0'   # Homebrew's vulkan-extensionlayer version
SRC = ROOT / 'dev-local/src/Vulkan-ExtensionLayer'
OUT = ROOT / 'dev-local/build/vulkan'
PATCH = ROOT / 'third_party/patches/vulkan-extensionlayer-shader-object-restart.patch'


def run(*cmd, cwd=None):
    if subprocess.run(cmd, cwd=cwd).returncode:
        sys.exit(f'failed: {" ".join(map(str, cmd))}')


def main():
    if not SRC.is_dir():
        run('git', 'clone', '-q', '--depth', '1', '--branch', TAG, 'https://github.com/KhronosGroup/Vulkan-ExtensionLayer.git', str(SRC))
    run('git', 'checkout', '-q', '--', '.', cwd=SRC)
    run('git', 'apply', str(PATCH), cwd=SRC)
    run('cmake', '-S', str(SRC), '-B', str(SRC / 'build'), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_PREFIX_PATH=/opt/homebrew', '-DBUILD_TESTS=OFF')
    run('ninja', '-C', str(SRC / 'build'), 'VkLayer_khronos_shader_object')
    OUT.mkdir(parents=True, exist_ok=True)
    lib = OUT / 'libVkLayer_khronos_shader_object.dylib'
    shutil.copy2(SRC / 'build/layers/libVkLayer_khronos_shader_object.dylib', lib)
    print(f'built {lib}')


if __name__ == '__main__':
    main()
