#!/usr/bin/env python3
"""Minimal `wget` for build scripts (macOS ships curl, not wget):
    wget [-q] [-O file] URL...      (and -qO-, --quiet, --no-check-certificate)"""
import os, sys, shutil, urllib.request
args, out, urls = sys.argv[1:], None, []
i = 0
while i < len(args):
    a = args[i]
    if a in ("-O", "--output-document"): out = args[i + 1]; i += 2; continue
    if a.startswith("-O") and len(a) > 2: out = a[2:]; i += 1; continue
    if a.startswith("-qO"): out = a[3:] or args[i + 1]; i += 1 if a[3:] else 2; continue
    if a.startswith("-"): i += 1; continue
    urls.append(a); i += 1
for u in urls:
    name = out or os.path.basename(u.split("?")[0]) or "index.html"
    req = urllib.request.Request(u, headers={"User-Agent": "Wget/1.21"})
    with urllib.request.urlopen(req, timeout=300) as r:
        if name == "-": shutil.copyfileobj(r, sys.stdout.buffer)
        else:
            with open(name, "wb") as f: shutil.copyfileobj(r, f)
