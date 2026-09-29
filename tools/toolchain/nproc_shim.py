#!/usr/bin/env python3
"""`nproc` (GNU coreutils) for macOS, where build scripts still call it."""
import os
print(os.cpu_count() or 1)
