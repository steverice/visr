#!/usr/bin/env python3
"""Merge recorded shader lists into the repository's (port/shader-lists).

The app appends the shaders and pipelines its draws made that a map's list did not have to MAP.txt in its
`debug.shader_list_record` folder (`shader-lists-missed` in its data folder; d3d8_device.c, "shader lists"), and
a run with `mac_run.py run --record-shader-lists` leaves them in runner/shader-lists in its --out folder. This adds
their lines to port/shader-lists/MAP.txt, keeping the lines already there, without duplicates, vertex shaders
first, then pixel shaders, then pipelines, each sorted. Each FOLDER is a run's --out folder or a folder of
MAP.txt files (a `shader-lists-missed` folder pulled from a device or a runner).

    python3 tools/shader_lists.py merge FOLDER [FOLDER...]
    python3 tools/shader_lists.py stats
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LISTS = ROOT / "port/shader-lists"
ORDER = {"vs": 0, "ps": 1, "pipeline": 2}
HEADER = ("# {map}: the shaders and pipelines this map's draws make, compiled while it loads\n"
          "# (d3d8_device.c, halo_shader_list_warm); merged by tools/shader_lists.py from recorded runs\n")


def read_lines(path):
    """the list's entries (no comments or blanks)"""
    if not path.is_file():
        return set()
    return {line.strip() for line in path.read_text().splitlines()
            if line.strip() and not line.startswith("#") and line.split()[0] in ORDER}


def write(lists, map_name, lines):
    ordered = sorted(lines, key=lambda line: (ORDER[line.split()[0]], line))
    (lists / f"{map_name}.txt").write_text(HEADER.format(map=map_name) + "".join(f"{line}\n" for line in ordered))


def recorded_lists(folder):
    """the MAP.txt files in a run's --out folder or in a folder of them"""
    folder = Path(folder)
    for candidate in (folder / "runner/shader-lists", folder):
        paths = sorted(candidate.glob("*.txt")) if candidate.is_dir() else []
        if paths:
            return paths
    sys.exit(f"{folder}: no recorded lists (neither runner/shader-lists/MAP.txt nor MAP.txt files)")


def merge(folders, lists=LISTS):
    """adds the folders' lines to the lists; returns how many each map gained"""
    lists.mkdir(parents=True, exist_ok=True)
    gained = {}
    for folder in folders:
        for path in recorded_lists(folder):
            existing = read_lines(lists / path.name)
            added = read_lines(path) - existing
            write(lists, path.stem, existing | added)
            gained[path.stem] = gained.get(path.stem, 0) + len(added)
            print(f"{path.stem}: {len(added)} lines added from {folder} ({len(existing | added)} in all)")
    return gained


def stats():
    for path in sorted(LISTS.glob("*.txt")):
        lines = read_lines(path)
        counts = {kind: sum(1 for line in lines if line.startswith(kind + " ")) for kind in ORDER}
        print(f"{path.stem}: {counts['vs']} vertex shaders, {counts['ps']} pixel shaders, "
              f"{counts['pipeline']} pipelines, {path.stat().st_size / 1024:.0f} KB")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("merge").add_argument("folders", nargs="+", type=Path)
    commands.add_parser("stats")
    args = parser.parse_args()
    if args.command == "merge":
        merge(args.folders)
    else:
        stats()


if __name__ == "__main__":
    main()
