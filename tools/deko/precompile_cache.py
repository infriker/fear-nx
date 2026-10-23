#!/usr/bin/env python3
"""Build portable Switch shader caches on a PC; never launches the game."""

import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--warmup-dir", type=Path, help="Captured fear-deko-warmup-v1 directory")
    parser.add_argument("--xsh", type=Path, help="Optional XESH cache (default variants only)")
    parser.add_argument("--output", type=Path, required=True, help="Output cache directory")
    parser.add_argument("--implicit-lod", choices=("true", "false"), default="true")
    parser.add_argument("--image", default="fear-nx-switch:devkitpro-20260219-clang19")
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    output = args.output.resolve()
    for name, path, is_dir in (("warmup-dir", args.warmup_dir, True), ("xsh", args.xsh, False)):
        if path is not None and not (path.is_dir() if is_dir else path.is_file()):
            parser.error(f"--{name} does not exist: {path}")
    output.mkdir(parents=True, exist_ok=True)

    def mount(path, target, readonly=False):
        return ["--mount", f"type=bind,source={path.resolve().as_posix()},target={target}"
                + (",readonly" if readonly else "")]

    docker = ["docker", "run", "--rm", *mount(root, "/project"), "-w", "/project"]

    def run(arguments, mounts=()):
        subprocess.run([*docker, *mounts, args.image, *arguments], check=True)

    build = "/project/out/deko-validation/build"
    if not args.skip_build:
        run(["cmake", "-S", "/project/tools/deko", "-B", build, "-G", "Ninja",
             "-DCMAKE_C_COMPILER=clang-19", "-DCMAKE_CXX_COMPILER=clang++-19",
             "-DCMAKE_BUILD_TYPE=Release"])
        run(["cmake", "--build", build, "--parallel", "4", "--target", "deko_shader_precompile"])
    mounts = mount(output, "/cache-output")
    arguments = [build + "/bin/deko_shader_precompile",
                 "--cache-dir", "/cache-output/fear-deko-prepared-v1",
                 "--internal-shaders", "--legacy-cache-dir", "/cache-output/fear-deko-dksh-v1",
                 "--implicit-lod", args.implicit_lod]
    if args.warmup_dir:
        mounts += mount(args.warmup_dir, "/shader-warmup", True)
        arguments += ["--warmup-dir", "/shader-warmup"]
    if args.xsh:
        mounts += mount(args.xsh, "/input.xsh", True)
        arguments += ["--xsh", "/input.xsh"]
    run(arguments, mounts)
    print(f"Prepared Switch cache: {output}")
    print("Copy both generated fear-deko-* folders into the game's cache directory.")
    print("Keep the captured fear-deko-warmup-v1 folder there for startup warmup.")


if __name__ == "__main__":
    main()
