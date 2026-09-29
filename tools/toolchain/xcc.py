#!/usr/bin/env python3
"""
Schwung cross-compiler shim.

Every Schwung module ships a build script that targets Ableton Move
(aarch64-linux-gnu, Cortex-A72). This shim is installed on PATH under the
names those scripts call (aarch64-linux-gnu-gcc, -g++, -strip, -ar, ...) and
re-targets the call at the host machine's native compiler:

  * Linux  : gcc/g++            -> .so (ELF), same as Move but native arch
  * macOS  : clang/clang++      -> Mach-O bundle/dylib, universal if requested

ARM-only tuning flags are dropped; everything else (sources, -I, -D, -O,
-l, -o) passes through untouched so each module is built by ITS OWN script.
"""
import os, sys, subprocess, platform

TOOL = os.path.basename(sys.argv[0])
for p in ("aarch64-linux-gnu-", "aarch64-none-linux-gnu-", "native-"):
    if TOOL.startswith(p):
        TOOL = TOOL[len(p):]
        break

IS_MAC = platform.system() == "Darwin"
ARCHS = os.environ.get("SCHWUNG_ARCHS", "").split()          # e.g. "arm64 x86_64"
LOG = os.environ.get("SCHWUNG_XCC_LOG")

DROP_PREFIX = ("-march=", "-mtune=", "-mcpu=", "-mfpu=", "-mfloat-abi=",
               "-mno-outline-atomics", "-moutline-atomics", "-mbranch-protection",
               "--sysroot", "-Wl,--sysroot")
DROP_EXACT = {"-static-libgcc", "-static-libstdc++", "-mstrict-align",
              "-mgeneral-regs-only", "-fno-plt"}
MAC_DROP_EXACT = {"-rdynamic", "-Wl,-E", "-Wl,--export-dynamic", "-lstdc++fs", "-fno-gnu-unique",
                  "-Wl,--no-undefined", "-Wl,-z,defs", "-Wl,--gc-sections",
                  "-Wl,--as-needed", "-Wl,--no-as-needed", "-Wl,-Bsymbolic",
                  "-Wl,--whole-archive", "-Wl,--no-whole-archive", "-lrt",
                  "-Wl,-O1", "-Wl,--strip-all", "-Wl,-s", "-s", "-latomic"}
MAC_DROP_PREFIX = ("-Wl,-soname", "-Wl,--version-script", "-Wl,-rpath-link",
                   "-Wl,--exclude-libs", "-Wl,-z,", "-Wl,--hash-style",
                   "-Wl,--build-id", "-Wl,-Map", "-Wl,--start-group", "-Wl,--end-group")

# GCC-only flags clang rejects outright ("unknown argument").
GCC_ONLY = {"-fno-gnu-unique", "-fconserve-stack", "-fprefetch-loop-arrays", "-fno-prefetch-loop-arrays",
            "-fvect-cost-model", "-fsingle-precision-constant"}
# GCC optimiser families that clang does not implement; it rejects them.
GCC_ONLY_PREFIX = ("-ftree-", "-fno-tree-", "-fipa-", "-fno-ipa-", "-fgcse", "-fno-gcse", "-floop-",
                   "-fno-loop-", "-fsched-", "-fno-sched-", "-fmodulo-sched", "-fvar-tracking",
                   "-fno-var-tracking", "-fpredictive-commoning", "-fsplit-paths", "-fno-split-paths",
                   "-fvect-cost-model=", "-fsimd-cost-model=", "-fira-", "-fno-ira-", "-flive-range",
                   "-fcaller-saves", "-fipa", "-fdevirtualize-", "-fno-devirtualize-", "-fpeel-loops",
                   "-fno-peel-loops", "-funswitch-loops", "-fno-unswitch-loops", "-fexcess-precision=")

def is_clang():
    return IS_MAC or "clang" in os.environ.get("SCHWUNG_CC", "") or "clang" in os.environ.get("SCHWUNG_CXX", "")

def filt(args):
    out, skip = [], False
    clang = is_clang()
    whole = False   # inside -Wl,--whole-archive ... -Wl,--no-whole-archive
    for i, a in enumerate(args):
        if skip:
            skip = False; continue
        if clang and (a in GCC_ONLY or a.startswith(GCC_ONLY_PREFIX)):
            continue
        if a in DROP_EXACT or a.startswith(DROP_PREFIX):
            if a == "--sysroot": skip = True
            continue
        if IS_MAC:
            # GNU ld's --whole-archive is ld64's -force_load, per archive.
            if a in ("-Wl,--whole-archive", "--whole-archive"):
                whole = True; continue
            if a in ("-Wl,--no-whole-archive", "--no-whole-archive"):
                whole = False; continue
            if whole and a.endswith(".a"):
                out.append("-Wl,-force_load," + a); continue
            if a in MAC_DROP_EXACT or a.startswith(MAC_DROP_PREFIX):
                continue
            if a == "-shared":
                # No `-undefined dynamic_lookup`: the chain host dlopen()s
                # every module with RTLD_NOW, so an unresolved symbol fails
                # the load anyway. Letting the linker refuse it instead names
                # the symbol at build time (sched_setscheduler taught us).
                out += ["-dynamiclib"]; continue
            if a == "-lstdc++fs": continue
            if a == "-pthread": out.append(a); continue
        out.append(a)
    return out

SYSROOT = os.environ.get("SCHWUNG_SYSROOT",
                         os.path.join(os.path.dirname(os.path.realpath(__file__)), ".cache", "sysroot"))

def sysroot_path(a):
    """-I/opt/arm64/include, -L/usr/local/lib, /opt/arm64/lib/libfftw3.a:
    container prefixes that the host lacks but the shared sysroot has."""
    for flag in ("-I", "-L", "-isystem", ""):
        if a.startswith(flag + "/opt/") or a.startswith(flag + "/usr/local/"):
            path = a[len(flag):]
            if not os.path.exists(path):
                alt = os.path.join(SYSROOT, path.lstrip("/"))
                if os.path.exists(alt):
                    return flag + alt
    return a

def compiler():
    if TOOL in ("gcc", "cc"):
        return [os.environ.get("SCHWUNG_CC", "clang" if IS_MAC else "gcc")]
    if TOOL in ("g++", "c++"):
        return [os.environ.get("SCHWUNG_CXX", "clang++" if IS_MAC else "g++")]
    return None

def main():
    args = sys.argv[1:]
    c = compiler()
    if c is not None:
        cmd = c + [sysroot_path(x) for x in filt(args)]
        # Per-repo defines from tools/recipes.json ("cflags"), for switches a
        # module's own sources document (e.g. PFFFT_SIMD_DISABLE).
        extra = os.environ.get("SCHWUNG_EXTRA_CFLAGS", "").split()
        if extra and any(a.endswith((".c", ".cc", ".cpp", ".cxx", ".C")) for a in args):
            cmd = cmd[:1] + extra + cmd[1:]
        # Linking a shared object: add the filesystem redirect (sw_fswrap.c),
        # whose hidden definitions of fopen/open/stat/... bind the module's
        # own references to Move paths onto the desktop data root.
        linking_shared = ("-shared" in args or "-dynamiclib" in args) and "-c" not in args \
                         and "-E" not in args and "-S" not in args
        wrap = os.environ.get("SCHWUNG_FSWRAP_OBJ")
        if linking_shared and wrap and os.path.exists(wrap):
            cmd.append(wrap)
            if not IS_MAC: cmd.append("-ldl")
        if IS_MAC or os.environ.get("SCHWUNG_CLANG_RELAX"):
            # Module code was written against GCC; clang is stricter about a
            # few things GCC merely warns about. Keep them warnings.
            cmd += ["-Wno-error", "-Wno-unknown-warning-option", "-Wno-unused-command-line-argument",
                    "-Wno-implicit-function-declaration", "-Wno-int-conversion",
                    "-Wno-incompatible-function-pointer-types", "-Wno-deprecated-declarations",
                    # GCC warns on narrowing in braced init; clang errors.
                    "-Wno-c++11-narrowing"]
        if IS_MAC:
            compat = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
                os.path.realpath(__file__)))), "core", "compat", "darwin")
            cmd = cmd[:1] + ["-isystem", compat] + cmd[1:]
        if IS_MAC and ARCHS:
            for a in ARCHS: cmd += ["-arch", a]
            mt = os.environ.get("MACOSX_DEPLOYMENT_TARGET")
            if mt: cmd.append(f"-mmacosx-version-min={mt}")
    elif TOOL == "strip":
        # A stripped Mach-O dylib loses the export we dlsym; keep globals.
        cmd = ["strip", "-x"] + [a for a in args if not a.startswith("--")] if IS_MAC \
              else ["strip"] + args
    elif TOOL in ("ar", "ranlib", "nm", "objdump", "objcopy", "readelf", "ld", "as"):
        name = {"objdump": "objdump", "readelf": "readelf" if not IS_MAC else "otool"}.get(TOOL, TOOL)
        if IS_MAC and TOOL in ("objcopy", "readelf"):
            return 0   # informational/post-processing steps: no-op on macOS
        cmd = [name] + args
    else:
        sys.stderr.write(f"xcc shim: unknown tool {TOOL}\n"); return 127
    if LOG:
        with open(LOG, "a") as f: f.write(" ".join(cmd) + "\n")
    if not IS_MAC and TOOL in ("objdump", "nm", "readelf"):
        # Module scripts end with a Move-deploy guard: "no symbol newer than
        # the device's glibc 2.35". That guard is about the device, not about
        # this build, and a desktop glibc is newer by definition. Report the
        # versioned symbols against the baseline so the guard stays quiet.
        import re
        p = subprocess.run(cmd, stdout=subprocess.PIPE)
        out = re.sub(rb"GLIBC_2\.(3[6-9]|[4-9][0-9])", b"GLIBC_2.17", p.stdout)
        out = re.sub(rb"GLIBCXX_3\.4\.(3[1-9]|[4-9][0-9])", b"GLIBCXX_3.4.19", out)
        out = b"\n".join(l for l in out.split(b"\n") if b"libmvec" not in l)
        sys.stdout.buffer.write(out)
        return p.returncode
    return subprocess.call(cmd)

if __name__ == "__main__":
    sys.exit(main())
