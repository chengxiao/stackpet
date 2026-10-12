#!/usr/bin/env python3
"""Give esp-dl's kernels that clash with another prebuilt library a private name (StackPet build step).

esp-sr ships a prebuilt old esp-dl (libdl_lib.a) whose assembly kernels have the same names as esp-dl 3.x's
(dl_tie728_s8_conv2d_33cn, dl_tie728_memcpy, ...) but take a different argument layout. The linker binds every
reference to whichever archive it meets first, esp-sr's: esp-dl's convolutions then run through the old kernels and
compute garbage (the face detector finds "faces" on a black image), while wake word / speech commands are fine.

This renames the clashing symbols inside the esp-dl archive only, definitions and esp-dl's own calls alike, so
each library keeps its own kernels. Idempotent: once renamed, nothing clashes any more and the archive is left as is.

With --check-dir it also fails the build when any other component library of the build references a renamed kernel
by its original name: that call would bind to the prebuilt copy instead of esp-dl's (checked on every link).

usage: espdl_unclash.py --nm NM --objcopy OBJCOPY --lib libespressif__esp-dl.a [--check-dir BUILD/esp-idf]
                        PREBUILT.a [PREBUILT.a ...]
"""
import argparse
import glob
import os
import subprocess
import sys
import tempfile

PREFIX = "stackpet_espdl_"


def defined_globals(nm, archive):
    out = subprocess.run([nm, "-g", "--defined-only", archive], check=True, capture_output=True, text=True).stdout
    symbols = set()
    for line in out.splitlines():
        parts = line.split()
        # "<address> <type> <name>" for symbols, "<member>:" / blank lines otherwise
        if len(parts) == 3 and parts[1] not in ("U", "w", "v"):
            symbols.add(parts[2])
    return symbols


def undefined_by_archive(nm, archives):
    """{archive: set of undefined symbols}, one nm call (-A prefixes every line with archive:member:)."""
    if not archives:
        return {}
    out = subprocess.run([nm, "-A", "-u", *archives], check=True, capture_output=True, text=True).stdout
    refs = {}
    for line in out.splitlines():
        head, _, rest = line.partition(":")
        parts = rest.split()
        if len(parts) >= 2 and parts[-2] in ("U", "w"):  # strong and weak undefined references
            refs.setdefault(head, set()).add(parts[-1])
    return refs


def owner_component(path):
    """managed_components/<component>/... -> <component> (the component that ships a prebuilt archive)."""
    parts = os.path.normpath(path).split(os.sep)
    if "managed_components" in parts:
        index = parts.index("managed_components")
        if index + 1 < len(parts):
            return parts[index + 1]
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nm", required=True)
    parser.add_argument("--objcopy", required=True)
    parser.add_argument("--lib", required=True, help="the esp-dl archive, modified in place")
    parser.add_argument("--check-dir", help="the build's esp-idf directory: its component libraries must not use "
                                            "the renamed kernels by their original names")
    parser.add_argument("others", nargs="+", help="prebuilt archives whose symbols esp-dl must not shadow or use")
    args = parser.parse_args()

    ours = defined_globals(args.nm, args.lib)
    theirs = set()
    for archive in args.others:
        if os.path.isfile(archive):
            theirs |= defined_globals(args.nm, archive)
    clashes = sorted(ours & theirs)
    if clashes:
        with tempfile.NamedTemporaryFile("w", suffix=".syms", delete=False) as table:
            for name in clashes:
                table.write(f"{name} {PREFIX}{name}\n")
            table_path = table.name
        try:
            subprocess.run([args.objcopy, f"--redefine-syms={table_path}", args.lib], check=True)
        finally:
            os.unlink(table_path)
        ours = defined_globals(args.nm, args.lib)
        left = ours & theirs
        if left:
            print(f"espdl_unclash: still clashing after the rename: {', '.join(sorted(left))}", file=sys.stderr)
            return 1
        print(f"espdl_unclash: renamed {len(clashes)} esp-dl symbols that clash with prebuilt libraries "
              f"({clashes[0]} ... {clashes[-1]})")

    if args.check_dir:
        renamed = {name[len(PREFIX):] for name in ours if name.startswith(PREFIX)}
        # The components shipping the prebuilt archives may call their own copies (their source-built glue)
        owners = {owner_component(archive) for archive in args.others} - {None}
        lib = os.path.realpath(args.lib)
        archives = [a for a in sorted(glob.glob(os.path.join(args.check_dir, "**", "*.a"), recursive=True))
                    if os.path.realpath(a) != lib
                    and os.path.relpath(a, args.check_dir).split(os.sep)[0] not in owners]
        bad = []
        for archive, symbols in undefined_by_archive(args.nm, archives).items():
            for name in sorted(symbols & renamed):
                bad.append(f"{os.path.relpath(archive, args.check_dir)} uses {name}")
        if bad:
            print("espdl_unclash: these libraries call esp-dl kernels by their original names, which now bind to a "
                  "prebuilt copy with another argument layout (call them through esp-dl, or rename them too):",
                  file=sys.stderr)
            for line in bad:
                print(f"  {line}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
