#!/usr/bin/env python3
"""Check Google formatting and braces without editing project or vendor files."""

import argparse
import json
from pathlib import Path
import shutil
import shlex
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--clang-format", default="clang-format-12")
    parser.add_argument("--clang-tidy", default="clang-tidy-12")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    build = Path(args.build_dir).resolve()
    for tool in (args.clang_format, args.clang_tidy):
        if not shutil.which(tool):
            parser.error(f"Required tool is unavailable: {tool}")
    database = build / "compile_commands.json"
    if not database.is_file():
        parser.error("Configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON first")
    sources = sorted(
        path for directory in ("include", "src", "tests", "examples", "benchmarks")
        for path in (root / directory).rglob("*")
        if path.suffix in (".h", ".cc")
    )
    subprocess.run(
        [args.clang_format, "--dry-run", "--Werror", *map(str, sources)],
        cwd=root, check=True,
    )
    entries = json.loads(database.read_text())
    units_in_database = {
        Path(entry["file"]).resolve() for entry in entries
        if Path(entry["file"]).resolve().is_relative_to(root)
        and Path(entry["file"]).resolve().relative_to(root).parts[0]
        in ("src", "tests", "examples", "benchmarks")
    }
    units = sorted(units_in_database | {
        path for path in sources if path.suffix == ".cc"
        and (path.relative_to(root).parts[0] == "benchmarks"
             or path.parent == root / "tests" / "consumer"
             or path.parent == root / "tests" / "toolchain")
    })
    fallback = next(entry for entry in entries
                    if Path(entry["file"]).name == "header_main.cc")
    command = fallback.get("arguments") or shlex.split(fallback["command"])
    fallback_flags = []
    skip_output = False
    for argument in command[1:]:
        if skip_output:
            skip_output = False
        elif argument == "-o":
            skip_output = True
        elif argument != "-c" and argument != fallback["file"]:
            fallback_flags.append(argument)
    failed = []
    log_path = build / "style-check.log"
    with log_path.open("w") as log:
        for unit in units:
            # GCC's default language mode need not match clang-tidy's default.
            invocation = [args.clang_tidy, str(unit),
                          f"--header-filter=^{root}/(include|src|tests|examples|benchmarks)/",
                          "--quiet"]
            if unit in units_in_database:
                invocation += ["-p", str(build), "--extra-arg=-std=c++17"]
            else:
                invocation += ["--", *fallback_flags, "-std=c++17"]
            result = subprocess.run(
                invocation,
                cwd=root, stdout=log, stderr=subprocess.STDOUT, check=False,
            )
            if result.returncode:
                failed.append(str(unit.relative_to(root)))
    if failed:
        print("Style diagnostics in:", ", ".join(failed), file=sys.stderr)
        print(f"Details: {log_path}", file=sys.stderr)
        return 1
    print(f"Passed: formatting {len(sources)} C++ files; "
          f"Google/brace checks for {len(units)} translation units")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as error:
        sys.exit(error.returncode)
