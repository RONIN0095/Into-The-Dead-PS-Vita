#!/usr/bin/env python3
"""Checks that every import of libunity.so/libil2cpp.so (docs/imports/*.txt)
has an entry in the loader's import tables (source/dynlib.c,
source/na_dynlib.c, source/na_android.h).

Imports without an entry end up as "Unresolved import" in so_util and crash
when called. GL/EGL functions fetched through eglGetProcAddress are not in
these lists (see source/na_gles3.c).

    python3 tools/check_imports.py
"""
import os
import re
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
tables = ["source/dynlib.c", "source/na_dynlib.c", "source/na_android.h"]
have = set()
for t in tables:
    with open(os.path.join(root, t)) as f:
        have |= set(re.findall(r'\{\s*"([A-Za-z_0-9]+)"\s*,', f.read()))

missing_any = False
for lib in ("libunity", "libil2cpp"):
    with open(os.path.join(root, "docs/imports", lib + ".txt")) as f:
        imports = [l.strip() for l in f if l.strip()]
    missing = [s for s in imports if s not in have]
    print(f"{lib}: {len(imports)} imports, {len(missing)} without an entry")
    for s in missing:
        print("   ", s)
    missing_any |= bool(missing)

sys.exit(1 if missing_any else 0)
