#!/usr/bin/env python3
"""Check that every undefined (non-weak) symbol of the APK's native libs resolves on a given API level.

Usage: check_api_syms.py <dir with the APK's .so files> <api level> [ndk]
Resolution follows the Android linker: a lib's NEEDED closure (bundled libs + NDK system stubs).
"""
import os, subprocess, sys
from collections import deque

libdir, api = sys.argv[1], sys.argv[2]
ndk = sys.argv[3] if len(sys.argv) > 3 else "/home/bilko/workspace/android-ndk-r30"
tc = f"{ndk}/toolchains/llvm/prebuilt/linux-x86_64"
nm, readelf = f"{tc}/bin/llvm-nm", f"{tc}/bin/llvm-readelf"
sysroot = f"{tc}/sysroot/usr/lib/aarch64-linux-android"
sysdir = f"{sysroot}/{api}"

def find(name):
    for d in (libdir, sysdir):
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    return None

_cache = {}
def info(path):
    if path in _cache:
        return _cache[path]
    needed = [l.split("[")[1].split("]")[0] for l in subprocess.run([readelf, "-d", path], capture_output=True, text=True).stdout.splitlines() if "(NEEDED)" in l]
    defined, undef = set(), set()
    for l in subprocess.run([nm, "-D", "--defined-only", path], capture_output=True, text=True).stdout.splitlines():
        parts = l.split()
        if len(parts) >= 3:
            defined.add(parts[2].split("@")[0])
    for l in subprocess.run([nm, "-D", "--undefined-only", path], capture_output=True, text=True).stdout.splitlines():
        parts = l.split()
        if len(parts) == 2 and parts[0] == "U":  # 'w' / 'v' = weak, allowed to stay unresolved
            undef.add(parts[1].split("@")[0])
    _cache[path] = (needed, defined, undef)
    return _cache[path]

bad = 0
for so in sorted(f for f in os.listdir(libdir) if f.endswith(".so")):
    path = os.path.join(libdir, so)
    needed, _, undef = info(path)
    # NEEDED closure, breadth-first
    seen, q, avail, missing_libs = set(), deque(needed), set(), []
    while q:
        n = q.popleft()
        if n in seen:
            continue
        seen.add(n)
        p = find(n)
        if not p:
            missing_libs.append(n)
            continue
        nd, df, _ = info(p)
        avail |= df
        q.extend(nd)
    unresolved = sorted(s for s in undef if s not in avail)
    if missing_libs or unresolved:
        bad += 1
        print(f"{so}: missing libs {missing_libs} unresolved {len(unresolved)}: {unresolved[:12]}")
print(f"checked against API {api}: {'OK' if not bad else str(bad) + ' libs with problems'}")
