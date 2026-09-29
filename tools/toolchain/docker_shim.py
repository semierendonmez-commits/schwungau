#!/usr/bin/env python3
"""
docker shim — a minimal, local stand-in for the Docker CLI as Schwung module
build scripts use it. There is no container and no image: the "container" is
the build context on disk, and container paths are mapped back onto it.

Supported, because module scripts use them:
  docker build [-t NAME] [-f Dockerfile] CONTEXT       (also `buildx build`)
      Parses the Dockerfile: COPY/ADD establish container-path -> host-path
      mappings, WORKDIR/ENV are tracked, and RUN steps that build something
      are executed locally (package-manager steps are skipped: the toolchain
      they install is our shim, already on PATH).
  docker run [opts] IMAGE [cmd...]          -v/-w/-e honoured; no cmd -> CMD
  docker create [opts] IMAGE [cmd...]       prints an id
  docker start [-a] ID / docker wait ID / docker inspect -f '{{.State.ExitCode}}' ID
  docker cp ID:/path host | host ID:/path
  anything else (image inspect, rm, rmi, pull, login ...) succeeds silently.
"""
import json, os, re, shlex, shutil, subprocess, sys, uuid

SYSROOT = os.environ.get("SCHWUNG_SYSROOT",
                         os.path.join(os.path.dirname(os.path.realpath(__file__)), ".cache", "sysroot"))
# Dependencies that a module's image builds (FFTW, libkeyfinder, ...) are
# installed under these prefixes; they persist here between builds.
SYSROOT_PREFIXES = ("/opt", "/usr/local")

def with_sysroot(maps):
    out = list(maps)
    for p in SYSROOT_PREFIXES:
        if host_path(out, p + "/x") is None:
            os.makedirs(os.path.join(SYSROOT, p.lstrip("/")), exist_ok=True)
            out.append([p, os.path.join(SYSROOT, p.lstrip("/"))])
    return out

STATE = os.environ.get("SCHWUNG_DOCKER_STATE",
                       os.path.join(os.path.dirname(os.path.realpath(__file__)), ".cache", "docker"))
os.makedirs(STATE, exist_ok=True)

SKIP_RUN = re.compile(r"(apt-get|apt |apk add|yum |dnf |pacman|useradd|groupadd|adduser|locale-gen|"
                      r"update-alternatives|dpkg|rustup|pip3? install|npm (ci|install)|curl .*\| *(ba)?sh)")

def jload(name, default):
    p = os.path.join(STATE, name + ".json")
    try:
        with open(p) as f: return json.load(f)
    except Exception: return default

def jsave(name, obj):
    with open(os.path.join(STATE, name + ".json"), "w") as f: json.dump(obj, f)

def mapper(maps):
    """maps: list of [container_path, host_path]. Longest prefix wins."""
    ms = sorted(maps, key=lambda m: -len(m[0]))
    def m(s):
        for c, h in ms:
            c = c.rstrip("/") or "/"
            if c == "/": continue
            # A container path starts a token, or follows a flag that glues
            # its argument on (-I/build/x, -L/build/lib, --prefix=/build).
            s = re.sub(r"(?:(?<=^)|(?<=[\s\"'=:;(,])|(?<=-I)|(?<=-L)|(?<=-B))" + re.escape(c) +
                       r"(?=/|\"|'|\s|$|;|\)|:)", h.replace("\\", "\\\\"), s)
        return s
    return m

def host_path(maps, cpath):
    for c, h in sorted(maps, key=lambda m: -len(m[0])):
        c = c.rstrip("/") or "/"
        if cpath == c or cpath.startswith(c + "/"):
            return h + cpath[len(c):]
    return None

def parse_dockerfile(path, context, exports=None):
    maps, env, workdir, cmd, steps = [], {}, "/", None, []
    if not path or not os.path.exists(path): return maps, env, workdir, cmd, steps
    stage = 0
    text = re.sub(r"\\\s*\n", " ", open(path, errors="ignore").read())
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"): continue
        parts = line.split(None, 1)
        ins, arg = parts[0].upper(), (parts[1] if len(parts) > 1 else "")
        if ins == "WORKDIR":
            workdir = arg if arg.startswith("/") else os.path.join(workdir, arg)
            if host_path(maps, workdir) is None:
                maps.append([workdir, context])
        elif ins == "ENV":
            if "=" in arg:
                for kv in shlex.split(arg):
                    k, _, v = kv.partition("="); env[k] = v
            else:
                k, _, v = arg.partition(" "); env[k] = v.strip()
        elif ins == "FROM":
            stage += 1
        elif ins in ("COPY", "ADD"):
            raw_toks = shlex.split(arg)
            from_stage = any(t.startswith("--from=") for t in raw_toks)
            toks = [t for t in raw_toks if not t.startswith("--")]
            if len(toks) < 2: continue
            *srcs, dst = toks
            if from_stage:
                # Multi-stage export (FROM scratch; COPY --from=builder x /):
                # remembered for `docker build --output type=local,dest=...`.
                if exports is not None:
                    for s in srcs: exports.append((s, dst))
                continue
            if not dst.startswith("/"): dst = os.path.join(workdir, dst)
            for s in srcs:
                hs = os.path.normpath(os.path.join(context, s))
                target = dst + os.path.basename(s) if dst.endswith("/") and os.path.isfile(hs) else (dst.rstrip("/") or "/")
                inside = host_path(maps, target)
                if s in (".", "./"):
                    maps.append([dst, context])
                elif inside and os.path.isfile(hs) and os.path.realpath(inside) != os.path.realpath(hs):
                    # Lands inside an already-mapped tree: later RUN steps
                    # address it by a relative path from that tree, so the
                    # file has to physically be there.
                    os.makedirs(os.path.dirname(inside), exist_ok=True)
                    shutil.copy2(hs, inside)
                else:
                    maps.append([target, hs])
        elif ins == "RUN":
            steps.append((workdir, arg, dict(env)))
        elif ins in ("CMD", "ENTRYPOINT"):
            try: cmd = json.loads(arg)
            except Exception: cmd = ["sh", "-c", arg]
    return maps, env, workdir, cmd, steps

def execute(cmd, maps, cwd_c, env_extra):
    maps = with_sysroot(maps)
    m = mapper(maps)
    cmd = [m(x) for x in cmd]
    cwd = host_path(maps, cwd_c) if cwd_c else None
    if not cwd or not os.path.isdir(cwd):
        cwd = maps[0][1] if maps else os.getcwd()
    e = dict(os.environ); e.update({k: m(v) for k, v in env_extra.items()})
    e["SCHWUNG_FAKE_DOCKER_DEPTH"] = str(int(os.environ.get("SCHWUNG_FAKE_DOCKER_DEPTH", "0")) + 1)
    if not cmd: return 0
    try:
        return subprocess.call(cmd, cwd=cwd, env=e)
    except OSError as ex:
        sys.stderr.write(f"docker shim: cannot run {cmd[:2]}: {ex}\n"); return 127

def parse_opts(a):
    vols, env, wd, entry, i = [], {}, None, None, 0
    NOARG = {"--rm", "-i", "-t", "-it", "-ti", "--init", "--privileged", "-d", "-a", "--attach", "--interactive"}
    WITHARG = {"-u", "--user", "--platform", "--name", "--network", "--memory", "--cpus", "--env-file",
               "-h", "--hostname", "--cidfile", "--ulimit", "--shm-size"}
    while i < len(a):
        x = a[i]
        if x in NOARG: i += 1; continue
        if x in ("-v", "--volume"):
            h, c = a[i + 1].split(":")[:2]; vols.append([c, os.path.abspath(h)]); i += 2; continue
        if x.startswith("-v") and ":" in x:
            h, c = x[2:].split(":")[:2]; vols.append([c, os.path.abspath(h)]); i += 1; continue
        if x in ("-w", "--workdir"): wd = a[i + 1]; i += 2; continue
        if x in ("-e", "--env"):
            k, s, v = a[i + 1].partition("="); env[k] = v if s else os.environ.get(k, ""); i += 2; continue
        if x == "--entrypoint": entry = a[i + 1]; i += 2; continue
        if x in WITHARG: i += 2; continue
        if x.startswith("-"): i += 1; continue
        break
    return vols, env, wd, entry, a[i] if i < len(a) else None, a[i + 1:]

def main():
    a = sys.argv[1:]
    if not a: return 0
    if int(os.environ.get("SCHWUNG_FAKE_DOCKER_DEPTH", "0")) >= 2:
        sys.stderr.write("docker shim: nested docker call refused\n"); return 97
    images = jload("images", {})
    conts = jload("containers", {})
    sub = a[0]
    if sub == "buildx" and len(a) > 1 and a[1] == "build": a = a[1:]; sub = "build"

    if sub == "build":
        tag, dfile, ctx, i, out = None, None, ".", 1, None
        while i < len(a):
            x = a[i]
            if x in ("-o", "--output"): out = a[i + 1]; i += 2; continue
            if x.startswith("--output="): out = x.split("=", 1)[1]; i += 1; continue
            if x in ("-t", "--tag"): tag = a[i + 1]; i += 2; continue
            if x in ("-f", "--file"): dfile = a[i + 1]; i += 2; continue
            if x in ("--build-arg", "--platform", "--target", "--progress", "--network"): i += 2; continue
            if x.startswith("-"): i += 1; continue
            ctx = x; i += 1
        ctx = os.path.abspath(ctx)
        dfile = os.path.abspath(dfile) if dfile else os.path.join(ctx, "Dockerfile")
        if not os.path.exists(dfile):
            # Real docker fails here too; staying quiet hid a wrong path once.
            sys.stderr.write(f"docker shim: Dockerfile not found: {dfile}\n")
            return 1
        exports = []
        maps, env, wd, cmd, steps = parse_dockerfile(dfile, ctx, exports)
        if not maps: maps = [[wd if wd != "/" else "/build", ctx]]
        for swd, run, senv in steps:
            if SKIP_RUN.search(run): continue
            rc = execute(["bash", "-c", run], maps, swd, senv)
            if rc != 0:
                sys.stderr.write(f"docker shim: RUN step failed ({rc}), continuing: {run[:100]}\n")
        if tag: images[tag.split(":")[0]] = {"maps": maps, "env": env, "workdir": wd, "cmd": cmd}
        jsave("images", images)
        if out:
            dest = dict(kv.split("=", 1) for kv in out.split(",") if "=" in kv).get("dest") if "=" in out else out
            if dest:
                os.makedirs(dest, exist_ok=True)
                for s, d in exports:
                    hp = host_path(maps, s)
                    if hp and os.path.exists(hp):
                        target = os.path.join(dest, d.lstrip("/")) if not d.endswith("/") and d != "/" else os.path.join(dest, d.lstrip("/"), os.path.basename(hp))
                        os.makedirs(os.path.dirname(target) or dest, exist_ok=True)
                        (shutil.copytree if os.path.isdir(hp) else shutil.copy2)(hp, target)
        return 0

    if sub in ("run", "create"):
        vols, env, wd, entry, image, cmd = parse_opts(a[1:])
        img = images.get((image or "").split(":")[0], {"maps": [], "env": {}, "workdir": "/build", "cmd": None})
        maps = vols + img["maps"]
        if not maps: maps = [["/build", os.getcwd()]]
        if entry: cmd = [entry] + cmd
        if not cmd: cmd = img.get("cmd") or []
        env2 = dict(img.get("env", {})); env2.update(env)
        wd = wd or img.get("workdir") or "/"
        if sub == "run":
            # `docker run IMAGE cp /build/x /out/x` where both mount the same
            # host directory is a copy onto itself: in a container these are
            # two filesystems, here they are one file.
            if len(cmd) == 3 and cmd[0] == "cp":
                s, d = host_path(maps, cmd[1]), host_path(maps, cmd[2])
                if s and d and os.path.exists(s):
                    dd = os.path.join(d, os.path.basename(s)) if os.path.isdir(d) else d
                    if os.path.exists(dd) and os.path.samefile(s, dd):
                        return 0
            return execute(cmd, maps, wd, env2)
        # A created container gets its OWN filesystem, as with real Docker:
        # the image's contents are copied into a scratch root, `docker cp`
        # writes there, and nothing touches the repo until a cp back out.
        # Bind mounts (-v) stay bound to the host.
        import tempfile
        root = tempfile.mkdtemp(prefix="sw_ctr_", dir=STATE)
        cmaps = [list(v) for v in vols]
        for c, h in img["maps"]:
            dst = os.path.join(root, c.lstrip("/"))
            if os.path.isdir(h):
                shutil.copytree(h, dst, symlinks=True, dirs_exist_ok=True,
                                ignore=shutil.ignore_patterns(".git", "sw_ctr_*"))
            elif os.path.isfile(h):
                os.makedirs(os.path.dirname(dst), exist_ok=True); shutil.copy2(h, dst)
            cmaps.append([c, dst])
        wdc = wd or "/build"
        if host_path(cmaps, wdc) is None:
            os.makedirs(os.path.join(root, wdc.lstrip("/")), exist_ok=True)
            cmaps.append([wdc, os.path.join(root, wdc.lstrip("/"))])
        cid = uuid.uuid4().hex[:12]
        conts[cid] = {"maps": cmaps, "root": root, "cmd": cmd, "env": env2, "workdir": wdc, "rc": None}
        jsave("containers", conts)
        print(cid)
        return 0

    if sub == "start":
        rc = 0
        for cid in [x for x in a[1:] if not x.startswith("-")]:
            c = conts.get(cid)
            if not c: continue
            c["rc"] = execute(c["cmd"], c["maps"], c["workdir"], c["env"])
            rc = rc or c["rc"]
        jsave("containers", conts)
        return rc

    if sub in ("wait", "inspect"):
        for cid in [x for x in a[1:] if not x.startswith("-") and not x.startswith("{{")]:
            if cid in conts:
                print(conts[cid]["rc"] if conts[cid]["rc"] is not None else 0)
        return 0

    if sub == "cp":
        srcs = [x for x in a[1:] if not x.startswith("-")]
        if len(srcs) != 2: return 1
        def resolve(p):
            if ":" in p and p.split(":", 1)[0] in conts:
                cid, cpath = p.split(":", 1)
                return host_path(conts[cid]["maps"], cpath)
            return os.path.abspath(p)
        # host -> container: copy into the container's own root.
        dst = srcs[1]
        if ":" in dst and dst.split(":", 1)[0] in conts and ":" not in srcs[0]:
            cid, cpath = dst.split(":", 1)
            c = conts[cid]
            hsrc = os.path.abspath(srcs[0])
            contents_only = srcs[0].endswith("/.")
            if contents_only: hsrc = hsrc[:-2] if hsrc.endswith("/.") else hsrc
            target = host_path(c["maps"], cpath.rstrip("/") or "/")
            if target is None and c.get("root"):
                target = os.path.join(c["root"], cpath.strip("/"))
                c["maps"].append([cpath.rstrip("/"), target])
            if os.path.isdir(hsrc):
                if not contents_only and cpath.endswith("/"):
                    target = os.path.join(target, os.path.basename(hsrc))
                shutil.copytree(hsrc, target, symlinks=True, dirs_exist_ok=True,
                                ignore=shutil.ignore_patterns(".git", "sw_ctr_*"))
            else:
                os.makedirs(os.path.dirname(target), exist_ok=True)
                shutil.copy2(hsrc, target if not cpath.endswith("/") else os.path.join(target, os.path.basename(hsrc)))
            jsave("containers", conts)
            return 0
        s, d = resolve(srcs[0]), resolve(srcs[1])
        if not s or not os.path.exists(s):
            sys.stderr.write(f"docker shim: cp source not found: {srcs[0]}\n"); return 1
        # Copying the build context into the "container" is a no-op here: the
        # container IS the build context. Also never copy a tree into itself.
        rs, rd = os.path.realpath(s.rstrip("/.") or s), os.path.realpath(d)
        if rs == rd or rd.startswith(rs + os.sep) or (os.path.exists(d) and os.path.samefile(s, d)):
            return 0
        if os.path.isdir(s):
            if os.path.isdir(d): d = os.path.join(d, os.path.basename(s.rstrip("/")))
            shutil.copytree(s, d, dirs_exist_ok=True)
        elif os.path.abspath(s) != os.path.abspath(d):
            os.makedirs(os.path.dirname(os.path.abspath(d)) or ".", exist_ok=True)
            shutil.copy2(s, d)
        return 0

    if sub == "rm":
        for cid in [x for x in a[1:] if not x.startswith("-")]:
            c = conts.pop(cid, None)
            if c and c.get("root"): shutil.rmtree(c["root"], ignore_errors=True)
        jsave("containers", conts)
        return 0
    return 0   # image inspect, rmi, pull, login, logs, ps ...

if __name__ == "__main__":
    sys.exit(main())
