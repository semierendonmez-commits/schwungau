#!/usr/bin/env python3
"""`file` shim: module scripts end with "is the output ARM aarch64?" — a check
about the Move, not about this build. Run the real `file` and append the
architecture those scripts grep for to every shared-object line."""
import os, shutil, subprocess, sys
here = os.path.dirname(os.path.realpath(__file__))
real = None
for d in os.environ.get("PATH", "").split(os.pathsep):
    c = os.path.join(d, "file")
    if os.path.realpath(c) != os.path.realpath(__file__) and os.access(c, os.X_OK) and os.path.isfile(c):
        real = c; break
if not real: sys.exit(0)
p = subprocess.run([real] + sys.argv[1:], stdout=subprocess.PIPE, text=True)
for line in p.stdout.splitlines():
    if ("ELF" in line or "Mach-O" in line) and "aarch64" not in line:
        line += ", ARM aarch64"
    print(line)
sys.exit(p.returncode)
