#!/usr/bin/env python3
"""Stage the pinned Deko3D UI/compiler sources and patch the private ReXGlue SDK copy."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import tempfile
from pathlib import Path


REXGLUE_REPOSITORY = "https://github.com/nx-mod/rexglue-sdk-nx.git"
REXGLUE_COMMIT = "7d19603450ca25eafcf05e1fdb7c9f77c044c852"
UAM_REPOSITORY = "https://github.com/centollOS/SwitchWakerHD.git"
UAM_COMMIT = "2d147aaaa27b1ad0f9b390eff515609af9fe24a3"
SPIRV_CROSS_REPOSITORY = "https://github.com/KhronosGroup/SPIRV-Cross.git"
SPIRV_CROSS_COMMIT = "f11ba9f0b21ba8fc15153d50a2a1ae31ab1cf8f7"


def run(*command: str, cwd: Path | None = None) -> None:
    subprocess.run(command, cwd=cwd, check=True)


def sparse_checkout(repository: str, commit: str, patterns: list[str], destination: Path) -> None:
    if destination.exists():
        shutil.rmtree(destination)
    with tempfile.TemporaryDirectory(prefix="fear-nx-deko-") as temporary:
        checkout = Path(temporary) / "source"
        checkout.mkdir()
        run("git", "init", str(checkout))
        run("git", "remote", "add", "origin", repository, cwd=checkout)
        run("git", "-c", "protocol.version=2", "fetch", "--quiet", "--depth=1", "--filter=blob:none",
            "origin", commit, cwd=checkout)
        run("git", "sparse-checkout", "init", "--no-cone", cwd=checkout)
        run("git", "sparse-checkout", "set", "--no-cone", *patterns, cwd=checkout)
        run("git", "checkout", "--detach", "FETCH_HEAD", cwd=checkout)
        shutil.copytree(checkout, destination, ignore=shutil.ignore_patterns(".git"), dirs_exist_ok=True)


def copy_pinned_sources(sdk: Path) -> None:
    deko_headers = sdk / "include/rex/ui/deko3d"
    deko_sources = sdk / "src/ui/deko3d"
    if not (deko_headers / "presentador.h").is_file() or not (deko_sources / "nucleo.cpp").is_file():
        with tempfile.TemporaryDirectory(prefix="fear-nx-rexglue-") as temporary:
            checkout = Path(temporary) / "source"
            sparse_checkout(
                REXGLUE_REPOSITORY,
                REXGLUE_COMMIT,
                ["src/ui/deko3d/**", "include/rex/ui/deko3d/**"],
                checkout,
            )
            for relative in ("src/ui/deko3d", "include/rex/ui/deko3d"):
                destination = sdk / relative
                if destination.exists():
                    shutil.rmtree(destination)
                shutil.copytree(checkout / relative, destination)

    if not (sdk / "thirdparty/uam/CMakeLists.txt").is_file():
        with tempfile.TemporaryDirectory(prefix="fear-nx-uam-") as temporary:
            checkout = Path(temporary) / "source"
            sparse_checkout(UAM_REPOSITORY, UAM_COMMIT, ["runtime/third_party/uam/**"], checkout)
            shutil.copytree(checkout / "runtime/third_party/uam", sdk / "thirdparty/uam")

    cross_root = sdk / "thirdparty/spirv-cross"
    if not (cross_root / "spirv_glsl.cpp").is_file():
        with tempfile.TemporaryDirectory(prefix="fear-nx-spirv-cross-") as temporary:
            checkout = Path(temporary) / "source"
            sparse_checkout(
                SPIRV_CROSS_REPOSITORY,
                SPIRV_CROSS_COMMIT,
                ["CMakeLists.txt", "cmake/**", "include/**", "spirv*.cpp", "spirv*.hpp",
                 "spirv.h", "GLSL.std.450.h", "LICENSE", "LICENSES/**", "pkg-config/**"],
                checkout,
            )
            shutil.copytree(checkout, cross_root, ignore=shutil.ignore_patterns(".git"), dirs_exist_ok=True)
    include_bridge = cross_root / "include/spirv_cross/spirv_glsl.hpp"
    if not include_bridge.is_file():
        include_bridge.parent.mkdir(parents=True, exist_ok=True)
        include_bridge.write_text(
            "#pragma once\n#include \"../../spirv_glsl.hpp\"\n", encoding="utf-8"
        )


def apply_sdk_patches(repo_root: Path, sdk: Path) -> None:
    patch_dir = repo_root / "patches/rexglue-sdk"
    commands = [
        (patch_dir / "0023-switch-deko3d-backend.patch", sdk / "CMakeLists.txt", "REXGLUE_USE_DEKO3D"),
        (patch_dir / "0024-switch-deko3d-compute-stage.patch",
         sdk / "include/rex/ui/deko3d/compilador.h", "kComputo"),
        (patch_dir / "0025-switch-nro-output-name.patch",
         sdk / "cmake/rexglue_switch.cmake", "ICON;OUTPUT_NAME"),
        (patch_dir / "0026-deko-image-allocation-retry.patch",
         sdk / "src/ui/deko3d/nucleo.cpp", "reserva de imagen fallida"),
        (patch_dir / "0027-switch-spirv-cross-headers.patch",
         sdk / "thirdparty/CMakeLists.txt", "target_include_directories(spirv-cross-core PRIVATE"),
        (patch_dir / "0028-switch-presenter-lifecycle.patch",
         sdk / "src/ui/deko3d/presentador.cpp",
         "~PresentadorDeko3d() override {\n    SetWindowSurfaceFromUIThread(nullptr, nullptr);"),
        (patch_dir / "0029-deko-translator-header-only-vulkan.patch",
         sdk / "src/graphics/CMakeLists.txt", "shared SPIR-V translator includes Vulkan-shaped headers"),
        (patch_dir / "0030-deko-test-heap-cap.patch",
         sdk / "src/ui/runtime_switch.cpp", "size_t __nx_heap_size = 1536ull * 1024 * 1024;"),
        (patch_dir / "0031-deko-enable-compute-queue.patch",
         sdk / "src/ui/deko3d/nucleo.cpp", "DkQueueFlags_Graphics | DkQueueFlags_Compute"),
        (patch_dir / "0032-deko-gpu-diagnostics.patch",
         sdk / "cmake/rexglue_switch.cmake", "option(REXGLUE_SWITCH_DEKO3D_DEBUG"),
        (patch_dir / "0033-prove-vertex-index-register.patch",
         sdk / "include/rex/graphics/pipeline/shader/shader.h",
         "vertex_fetch_indices_unmodified() const"),
    ]
    for patch_file, marker_file, marker in commands:
        if marker in marker_file.read_text(encoding="utf-8", errors="ignore"):
            continue
        run("patch", "--batch", "--forward", "--dry-run", "-p1", "-d", str(sdk), "-i", str(patch_file))
        run("patch", "--batch", "--forward", "-p1", "-d", str(sdk), "-i", str(patch_file))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--sdk-dir", type=Path)
    arguments = parser.parse_args()

    repo_root = arguments.repo_root.resolve()
    sdk = (arguments.sdk_dir or repo_root / "third_party/rexglue-sdk").resolve()
    if not (sdk / "CMakeLists.txt").is_file():
        raise SystemExit(f"ReXGlue SDK not found at {sdk}; prepare the pinned 0.10.0 SDK first.")
    sdk_cmake = (sdk / "CMakeLists.txt").read_text(encoding="utf-8", errors="ignore")
    if not re.search(r"project\(ReXGlue\s+VERSION\s+0\.10\.0", sdk_cmake):
        raise SystemExit(f"Expected the ReXGlue SDK 0.10.0 source tree at {sdk}.")
    if (sdk / ".git").exists():
        head = subprocess.check_output(["git", "-C", str(sdk), "rev-parse", "HEAD"], text=True).strip()
        if head != "c94f5ebdcb3c9d1a460ca48e04f9758448f8d518":
            raise SystemExit(
                f"Expected ReXGlue 0.10.0 base c94f5ebdcb3c9d1a460ca48e04f9758448f8d518, found {head}."
            )

    copy_pinned_sources(sdk)
    apply_sdk_patches(repo_root, sdk)
    print(f"Prepared Deko3D dependencies in {sdk}")
    print(f"ReXGlue Deko source: {REXGLUE_COMMIT}")
    print(f"UAM: {UAM_COMMIT}; SPIRV-Cross: {SPIRV_CROSS_COMMIT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
