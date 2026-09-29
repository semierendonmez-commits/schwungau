#!/usr/bin/env python3
"""
Build every Schwung module with its OWN build script, re-targeted at the host
machine through tools/toolchain (see xcc.py), then install the results into a
Move-shaped module tree:

    <out>/modules/sound_generators/<id>/dsp.so
    <out>/modules/audio_fx/<id>/<id>.so
    <out>/modules/midi_fx/<id>/dsp.so
    <out>/modules/{tools,overtake,utilities}/<id>/...

That is the exact layout the Schwung chain host resolves paths against, so the
shell can run the chain host unmodified.

Usage:
    build_modules.py --src mods/ --catalog module-catalog.json --out build/rootfs \
                     [--only id,id] [--skip id,id] [--jobs N] [--timeout S]
"""
import argparse, json, os, shutil, subprocess, sys, time, glob, concurrent.futures as cf

HERE = os.path.dirname(os.path.abspath(__file__))
SHIM_BIN = os.path.join(HERE, "toolchain", "bin")
FSWRAP_OBJ = os.path.join(HERE, "toolchain", ".cache", "sw_fswrap.o")

def build_fswrap():
    """Compile core/src/sw_fswrap.c once for this machine (and arch set)."""
    import platform
    src = os.path.join(os.path.dirname(HERE), "core", "src", "sw_fswrap.c")
    os.makedirs(os.path.dirname(FSWRAP_OBJ), exist_ok=True)
    cc = "clang" if platform.system() == "Darwin" else "gcc"
    cmd = [cc, "-O2", "-fPIC", "-fvisibility=hidden", "-c", src, "-o", FSWRAP_OBJ]
    for a in os.environ.get("SCHWUNG_ARCHS", "").split():
        cmd += ["-arch", a]
    subprocess.check_call(cmd)

TYPE_DIR = {
    "sound_generator": "sound_generators",
    "audio_fx": "audio_fx",
    "midi_fx": "midi_fx",
    "tool": "tools",
    "overtake": "overtake",
    "utility": "utilities",
}

RECIPES = {}
def load_recipes():
    global RECIPES
    p = os.path.join(HERE, "recipes.json")
    if os.path.exists(p):
        RECIPES = {k: v for k, v in json.load(open(p)).items() if not k.startswith("_")}

def find_build_script(repo):
    name = os.path.basename(repo).replace("__", "/", 1)
    r = RECIPES.get(name, {})
    if r.get("script"):
        p = os.path.join(repo, r["script"])
        return p if os.path.exists(p) else None
    for rel in ("scripts/build.sh", "build.sh", "build-module.sh", "build-dsp.sh",
                "scripts/build_dsp.sh", "scripts/build-module.sh"):
        p = os.path.join(repo, rel)
        if os.path.exists(p):
            return p
    return None

def run_build(repo, log_path, timeout):
    recipe = RECIPES.get(os.path.basename(repo).replace("__", "/", 1), {})
    if recipe.get("skip"):
        return "skipped: " + recipe["skip"]
    if recipe.get("commands"):
        script = os.path.join(repo, ".schwung_recipe.sh")
        with open(script, "w") as f:
            f.write("set -e\n" + "\n".join(recipe["commands"]) + "\n")
    else:
        script = find_build_script(repo)
    if script is None:
        return "no-build-script"
    missing = [t for t in recipe.get("requires", []) if not shutil.which(t)]
    if missing:
        return "missing-tools: " + ",".join(missing)
    env = dict(os.environ)
    env["PATH"] = SHIM_BIN + os.pathsep + env["PATH"]
    env["CROSS_PREFIX"] = os.path.join(SHIM_BIN, "aarch64-linux-gnu-")
    env["CROSS_COMPILE"] = env["CROSS_PREFIX"]
    env["CC"] = env["CROSS_PREFIX"] + "gcc"
    env["CXX"] = env["CROSS_PREFIX"] + "g++"
    env["SCHWUNG_NATIVE_BUILD"] = "1"
    env["SCHWUNG_FSWRAP_OBJ"] = FSWRAP_OBJ
    # Some repos ship helper scripts without the exec bit.
    for dp, _, fs in os.walk(repo):
        if "/." in dp: continue
        for f in fs:
            if f.endswith(".sh"):
                try: os.chmod(os.path.join(dp, f), 0o755)
                except OSError: pass
    env.setdefault("MAKEFLAGS", f"-j{os.cpu_count() or 1}")
    # Scripts decide "am I in the build container?" by testing /.dockerenv.
    # Run a throw-away copy whose test points at a marker file that exists,
    # so they take their in-container path: the one that calls the
    # aarch64-linux-gnu-* toolchain directly, which is our shim.
    marker = os.path.join(HERE, "toolchain", ".dockerenv")
    open(marker, "a").close()
    text = open(script, errors="ignore").read()
    run_script = script
    if "/.dockerenv" in text:
        run_script = os.path.join(os.path.dirname(script), ".schwung_native_" + os.path.basename(script))
        with open(run_script, "w") as f:
            f.write(text.replace("/.dockerenv", marker))
        os.chmod(run_script, 0o755)
    env["NATIVE"] = env.get("NATIVE", "0")
    env.update({k: str(v) for k, v in recipe.get("env", {}).items()})
    env["SCHWUNG_EXTRA_CFLAGS"] = " ".join(recipe.get("cflags", []))
    # Some images build dependencies (FFTW, libkeyfinder...) in their
    # Dockerfile; the in-container script then expects them under /opt.
    # Run that Dockerfile once through the emulator: its installs land in
    # the shared sysroot, which the compiler shim resolves /opt paths to.
    pre = recipe.get("prebuild_docker")
    if pre:
        repo = os.path.abspath(repo)
        # Done when its declared product is in the sysroot, not when a stamp
        # says so: a run that installed nothing must not count as done.
        sysroot = os.path.join(HERE, "toolchain", ".cache", "sysroot")
        product = os.path.join(sysroot, recipe.get("prebuild_product", "").lstrip("/"))
        if not recipe.get("prebuild_product") or not os.path.exists(product):
            with open(log_path + ".prebuild", "w") as log:
                subprocess.run([os.path.join(SHIM_BIN, "docker"), "build", "-t",
                                "sw-prebuild-" + os.path.basename(repo).lower(),
                                "-f", os.path.join(repo, pre[0]), os.path.join(repo, pre[1])],
                               cwd=repo, env=env, stdout=log, stderr=subprocess.STDOUT)
            if recipe.get("prebuild_product") and not os.path.exists(product):
                return "prebuild-failed (see .prebuild log)"
    # Host-side steps a script only runs outside its container (we run it
    # "inside"): e.g. staging a UI bundle before the compile.
    for c in recipe.get("pre_commands", []):
        subprocess.run(["bash", "-c", c], cwd=repo, env=env)
    t0 = time.time()
    with open(log_path, "w") as log:
        try:
            p = subprocess.run(["bash", os.path.relpath(run_script, repo)] + recipe.get("args", []),
                               cwd=repo, env=env,
                               stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
            rc = p.returncode
        except subprocess.TimeoutExpired:
            return "timeout"
    # Repos whose script leaves the binary somewhere of its own: assemble
    # dist/<id>/ from the recipe's "package" map (dest name -> repo path).
    for mod_id, files in recipe.get("package", {}).items():
        dist = os.path.join(repo, "dist", mod_id)
        os.makedirs(dist, exist_ok=True)
        for dest, src in files.items():
            sp = os.path.join(repo, src)
            if os.path.exists(sp):
                shutil.copy2(sp, os.path.join(dist, dest))
    return "ok" if rc == 0 else f"rc={rc} ({time.time()-t0:.0f}s)"

def locate_module_dir(repo, mod_id, newer_than=0.0):
    """Find the packaged module directory (dist/<id>/ with module.json)."""
    cands = []
    for mj in glob.glob(os.path.join(repo, "**", "module.json"), recursive=True):
        d = os.path.dirname(mj)
        try:
            j = json.load(open(mj))
        except Exception:
            continue
        if j.get("id") != mod_id:
            continue
        # Only a binary produced by THIS build counts: a repo keeps outputs
        # of earlier builds (other compiler, other arch), and installing one
        # of those would report a failed build as a success.
        has_bin = any(f.endswith((".so", ".dylib")) and os.path.getmtime(os.path.join(d, f)) >= newer_than
                      for f in os.listdir(d))
        score = (has_bin, "/dist/" in d, "/build/" in d)
        cands.append((score, d))
    if not cands:
        return None
    cands.sort(reverse=True)
    best = cands[0]
    if not best[0][0]:
        # No built binary next to any module.json with this id. A source-tree
        # module.json (src/) is not an installable module; JS-only modules are
        # the exception and are recognised by declaring no "dsp".
        try:
            j = json.load(open(os.path.join(best[1], "module.json")))
        except Exception:
            return None
        if j.get("dsp") or j.get("component_type") in ("sound_generator", "audio_fx", "midi_fx"):
            return None
    return best[1]

def install(mod, repo, root, newer_than=0.0):
    src = locate_module_dir(repo, mod["id"], newer_than)
    if not src:
        return None, "not-built"
    tdir = TYPE_DIR.get(mod["component_type"], "other")
    dst = os.path.join(root, "modules", tdir, mod["id"])
    if os.path.exists(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst, symlinks=True, ignore_dangling_symlinks=True,
                    ignore=shutil.ignore_patterns("*.o", ".git", "node_modules", "*.tar.gz"))
    bins = [f for f in os.listdir(dst) if f.endswith((".so", ".dylib"))]
    # The chain host opens fixed names; make sure they exist.
    want = {"sound_generator": "dsp.so", "midi_fx": "dsp.so",
            "audio_fx": f"{mod['id']}.so"}.get(mod["component_type"])
    if want and want not in bins and bins:
        shutil.copy2(os.path.join(dst, bins[0]), os.path.join(dst, want))
        bins.append(want)
    return dst, ("ok" if bins else "no-binary (js-only?)")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--catalog", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default="")
    ap.add_argument("--skip", default="")
    ap.add_argument("--jobs", type=int, default=1)
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--schwung", default="", help="Schwung host checkout (headers)")
    ap.add_argument("--resume", action="store_true", help="skip repos already built ok (results.jsonl)")
    ap.add_argument("--no-build", action="store_true", help="only (re)install existing outputs")
    a = ap.parse_args()

    if a.schwung:
        sw = os.path.abspath(a.schwung)
        os.environ["MOVE_ANYTHING_SRC"] = os.path.join(sw, "src")
        os.environ["SCHWUNG_SRC"] = os.path.join(sw, "src")
        # Scripts also look for a sibling checkout next to the module repo.
        for name in ("schwung", "move-anything", "move-everything"):
            link = os.path.join(os.path.abspath(a.src), name)
            if not os.path.exists(link):
                os.symlink(sw, link)
    build_fswrap()
    load_recipes()
    cat = json.load(open(a.catalog))
    mods = cat["modules"]
    only = set(filter(None, a.only.split(",")))
    skip = set(filter(None, a.skip.split(",")))
    if only:
        mods = [m for m in mods if m["id"] in only]
    mods = [m for m in mods if m["id"] not in skip]

    by_repo = {}
    for m in mods:
        by_repo.setdefault(m["github_repo"], []).append(m)

    os.makedirs(os.path.join(a.out, "logs"), exist_ok=True)
    results = {}

    def work(repo_name):
        repo = os.path.join(a.src, repo_name.replace("/", "__"))
        if not os.path.isdir(repo):
            return repo_name, "missing-source", []
        t_start = time.time() - 2
        st = "skipped-build" if a.no_build else run_build(
            repo, os.path.join(a.out, "logs", repo_name.replace("/", "__") + ".log"), a.timeout)
        newer = 0.0 if a.no_build else t_start
        inst = []
        for m in by_repo[repo_name]:
            try:
                d, s = install(m, repo, a.out, newer)
            except Exception as ex:
                s = "install-error: " + str(ex)[:120]
            inst.append((m["id"], s))
        return repo_name, st, inst

    jl = os.path.join(a.out, "results.jsonl")
    done = set()
    if a.resume and os.path.exists(jl):
        for line in open(jl):
            try:
                r = json.loads(line)
            except Exception:
                continue
            if r["build"] == "ok" or all(v == "ok" for v in r["modules"].values()):
                done.add(r["repo"])
    todo = [r for r in sorted(by_repo) if r not in done]
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for repo_name, st, inst in ex.map(work, todo):
            results[repo_name] = {"build": st, "modules": dict(inst)}
            with open(jl, "a") as f:
                f.write(json.dumps({"repo": repo_name, "build": st, "modules": dict(inst)}) + "\n")
            print(f"{repo_name:55} {st:22} " + " ".join(f"{i}:{s}" for i, s in inst), flush=True)

    json.dump(results, open(os.path.join(a.out, "build_report.json"), "w"), indent=1)

if __name__ == "__main__":
    main()
