#!/usr/bin/env python3
"""
IOS.py — patches the Unity Mono source tree for iOS JIT support.

Reads IOS-JIT.cpp from the same directory and installs it as
mono/utils/mono-ios-jit.h, then patches the build configuration
and runtime source files so the dual-mapped JIT pool is used.

Usage: python3 IOS.py <path-to-mono-repo-root>
"""

import os
import re
import sys
import shutil

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
JIT_SRC    = os.path.join(SCRIPT_DIR, "IOS-JIT.cpp")

# Files in the Mono source tree that need the JIT header.
INCLUDE_TARGETS = [
    "mono/mini/mini-runtime.c",
    "mono/utils/mono-mmap.c",
]

INCLUDE_LINE = '#include "mono-ios-jit.h"\n'


def read_jit_source():
    with open(JIT_SRC, "r") as f:
        return f.read()


def install_header(dest_root):
    """Copy IOS-JIT.cpp as mono-ios-jit.h into the Mono utils directory."""
    header_dest = os.path.join(dest_root, "mono", "utils", "mono-ios-jit.h")
    os.makedirs(os.path.dirname(header_dest), exist_ok=True)
    shutil.copy2(JIT_SRC, header_dest)
    print(f"[IOS.py] installed JIT header -> {header_dest}")


def patch_file(path):
    """Insert the JIT include into a Mono source file if not present."""
    if not os.path.isfile(path):
        print(f"[IOS.py] skipping missing: {path}")
        return

    with open(path, "r") as f:
        content = f.read()

    if INCLUDE_LINE in content:
        print(f"[IOS.py] already patched: {path}")
        return

    lines = content.splitlines(keepends=True)
    insert_at = 0
    for i, line in enumerate(lines):
        if line.startswith("#include"):
            insert_at = i + 1

    lines.insert(insert_at, INCLUDE_LINE)
    content = "".join(lines)

    with open(path, "w") as f:
        f.write(content)

    print(f"[IOS.py] patched: {path}")


def patch_ios_mk(dest_root):
    """Enable JIT in the iOS build configuration."""
    ios_mk = os.path.join(dest_root, "sdks", "builds", "ios.mk")
    if not os.path.isfile(ios_mk):
        print(f"[IOS.py] ios.mk not found at {ios_mk}")
        return

    with open(ios_mk, "r") as f:
        content = f.read()

    # Ensure JIT is not disabled.
    content = content.replace("-DDISABLE_JIT", "")
    content = content.replace("DISABLE_JIT=1", "DISABLE_JIT=0")

    with open(ios_mk, "w") as f:
        f.write(content)

    print(f"[IOS.py] patched ios.mk -> JIT enabled")


def main():
    if not os.path.isfile(JIT_SRC):
        print(f"[IOS.py] ERROR: {JIT_SRC} not found")
        sys.exit(1)

    dest_root = sys.argv[1] if len(sys.argv) > 1 else "."

    install_header(dest_root)

    for rel in INCLUDE_TARGETS:
        patch_file(os.path.join(dest_root, rel))

    patch_ios_mk(dest_root)

    print("[IOS.py] done.")


if __name__ == "__main__":
    main()
