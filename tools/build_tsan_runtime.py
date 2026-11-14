#!/usr/bin/env python3
"""Build a pinned, TSan-instrumented libc++/libc++abi in a local build directory."""

import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import urllib.request


VERSION = "12.0.1"
ARCHIVES = {
    "llvm": "7d9a8405f557cefc5a21bf5672af73903b64749d9bc3a50322239f56f34ffddf",
    "libcxx": "a42089cd358f661823c490741f8be98701d983a7ee5152c8649075da681a9d15",
    "libcxxabi": "88efe8e391767a1e8f42b509879abe766c9f44b1781ad1900975ae6b516b91d0",
}


def prepare_sources(root):
    root.mkdir(parents=True, exist_ok=True)
    for project, expected in ARCHIVES.items():
        filename = f"{project}-{VERSION}.src.tar.xz"
        archive = root / filename
        if not archive.exists():
            url = ("https://github.com/llvm/llvm-project/releases/download/"
                   f"llvmorg-{VERSION}/{filename}")
            temporary = archive.with_suffix(".download")
            with urllib.request.urlopen(url, timeout=60) as source:
                with temporary.open("wb") as output:
                    shutil.copyfileobj(source, output)
            temporary.rename(archive)
        actual = hashlib.sha256(archive.read_bytes()).hexdigest()
        if actual != expected:
            raise RuntimeError(f"Checksum mismatch: {archive}")
        directory = root / f"{project}-{VERSION}.src"
        if not directory.exists():
            with tarfile.open(archive) as bundle:
                members = bundle.getmembers()
                if project == "llvm":
                    # Standalone runtime builds require LLVM's matching CMake
                    # modules, not a newly built LLVM compiler or its sources.
                    members = [entry for entry in members
                               if entry.name.startswith(f"llvm-{VERSION}.src/cmake/")]
                for entry in members:
                    target = (root / entry.name).resolve()
                    if not target.is_relative_to(root.resolve()):
                        raise RuntimeError("Archive member escapes source directory")
                    if entry.issym() or entry.islnk():
                        base = target.parent if entry.issym() else root
                        if not (base / entry.linkname).resolve().is_relative_to(root.resolve()):
                            raise RuntimeError("Archive link escapes source directory")
                    elif not (entry.isfile() or entry.isdir()):
                        raise RuntimeError("Unsupported archive member type")
                bundle.extractall(root, members=members)
        alias = root / project
        if not alias.exists():
            alias.symlink_to(directory.name, target_is_directory=True)
        elif alias.resolve() != directory.resolve():
            raise RuntimeError(f"Unexpected source path already exists: {alias}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path,
                        default=Path(__file__).resolve().parent.parent / "build-toolchain")
    parser.add_argument("--clang", default="clang-12")
    parser.add_argument("--clangxx", default="clang++-12")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    for tool in ("cmake", args.clang, args.clangxx):
        if not shutil.which(tool):
            parser.error(f"Required tool is unavailable: {tool}")
    work = args.work_dir.resolve()
    sources = work / "tsan-sources"
    prefix = work / "tsan-runtime"
    prepare_sources(sources)
    common = [
        f"-DCMAKE_C_COMPILER={args.clang}",
        f"-DCMAKE_CXX_COMPILER={args.clangxx}",
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
        f"-DCMAKE_INSTALL_PREFIX={prefix}",
        "-DCMAKE_CXX_FLAGS=-fsanitize=thread -fno-omit-frame-pointer",
        "-DCMAKE_C_FLAGS=-fsanitize=thread -fno-omit-frame-pointer",
        "-DLLVM_INCLUDE_TESTS=OFF", f"-DLLVM_PATH={sources / 'llvm'}",
    ]
    projects = [
        ("libcxxabi", "tsan-abi-build", [
            "-DLIBCXXABI_ENABLE_STATIC=OFF", "-DLIBCXXABI_INCLUDE_TESTS=OFF",
            f"-DLIBCXXABI_LIBCXX_PATH={sources / 'libcxx'}",
        ]),
        ("libcxx", "tsan-cxx-build", [
            # LLVM 12 emits a libc++.so linker script. CMake must not attempt
            # ELF RPATH rewriting on that text file during installation.
            "-DCMAKE_SKIP_RPATH=ON",
            "-DLLVM_USE_SANITIZER=Thread", "-DLIBCXX_CXX_ABI=libcxxabi",
            f"-DLIBCXX_CXX_ABI_INCLUDE_PATHS={sources / 'libcxxabi' / 'include'}",
            f"-DLIBCXX_CXX_ABI_LIBRARY_PATH={prefix / 'lib'}",
            "-DLIBCXX_ENABLE_STATIC=OFF", "-DLIBCXX_ENABLE_EXPERIMENTAL_LIBRARY=OFF",
            "-DLIBCXX_INCLUDE_TESTS=OFF", "-DLIBCXX_INCLUDE_BENCHMARKS=OFF",
        ]),
    ]
    for project, directory, options in projects:
        build = work / directory
        commands = [
            ["cmake", "-S", str(sources / project), "-B", str(build), *common, *options],
            ["cmake", "--build", str(build), "--parallel", str(args.jobs)],
            ["cmake", "--install", str(build)],
        ]
        for command in commands:
            subprocess.run(command, check=True)
    print(f"Instrumented runtime installed locally: {prefix}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
