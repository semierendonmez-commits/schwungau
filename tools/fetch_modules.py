#!/usr/bin/env python3
"""
Fetch every module repository in module-catalog.json at the commit pinned in
modules.lock.json (or at HEAD with --latest), with submodules.

    tools/fetch_modules.py --dest mods/ [--latest] [--only id,id] [--jobs 4]
"""
import argparse, json, os, subprocess, concurrent.futures as cf

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def git(*a, cwd=None):
    return subprocess.run(["git", *a], cwd=cwd, capture_output=True, text=True)

def fetch(repo, commit, dest):
    d = os.path.join(dest, repo.replace("/", "__"))
    url = f"https://github.com/{repo}.git"
    if not os.path.isdir(os.path.join(d, ".git")):
        os.makedirs(d, exist_ok=True)
        git("init", "-q", cwd=d)
        git("remote", "add", "origin", url, cwd=d)
    ref = commit or "HEAD"
    r = git("fetch", "-q", "--depth", "1", "origin", ref, cwd=d)
    if r.returncode != 0:
        return repo, "fetch failed: " + r.stderr.strip()[-160:]
    git("checkout", "-q", "-f", "FETCH_HEAD", cwd=d)
    git("submodule", "update", "-q", "--init", "--recursive", "--depth", "1", cwd=d)
    return repo, "ok " + git("rev-parse", "--short", "HEAD", cwd=d).stdout.strip()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dest", required=True)
    ap.add_argument("--latest", action="store_true", help="ignore the lock file, take each repo's HEAD")
    ap.add_argument("--only", default="")
    ap.add_argument("--jobs", type=int, default=4)
    a = ap.parse_args()
    cat = json.load(open(os.path.join(ROOT, "module-catalog.json")))
    lock = json.load(open(os.path.join(ROOT, "modules.lock.json")))
    only = set(filter(None, a.only.split(",")))
    repos = sorted({m["github_repo"] for m in cat["modules"] if not only or m["id"] in only})
    os.makedirs(a.dest, exist_ok=True)
    jobs = [(r, None if a.latest else lock["modules"].get(r)) for r in repos]
    sw = lock.get("schwung", {})
    jobs.append((sw.get("repo", "charlesvestal/schwung"), None if a.latest else sw.get("commit")))
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for repo, st in ex.map(lambda j: fetch(j[0], j[1], a.dest), jobs):
            print(f"{repo:60} {st}", flush=True)

if __name__ == "__main__":
    main()
