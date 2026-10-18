#!/usr/bin/env bash
#
# Runs the `build` job of .github/workflows/ci.yml locally, inside the same
# toolchain image, step by step.
#
# The point is fidelity to one thing in particular: the shell. A container job
# gets `sh -e` by default and the runner itself gets `bash -e`, and `sh` on
# Ubuntu is dash, which has no `pipefail`. A hand-written check that runs the
# steps under a shell of its own choosing proves nothing about either, and one
# that ran them under bash is exactly why a broken test step reached CI.
#
# So the shell comes from the workflow: whatever `shell:` says, as GitHub
# spells it. The steps come from the workflow too, in order, with their `env:`.
#
# The source tree is `git archive HEAD`, not the working tree, so uncommitted
# files and build output cannot make a run pass that CI would fail.
#
# What this does NOT reproduce: the `shim` and `toolchain` jobs, anything
# needing a GitHub token, `actions/*` steps (checkout is the export), and
# expression contexts beyond the two outputs the build job consumes.
#
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"
REV=$(git rev-parse HEAD:llvm)
IMG="${WTC_IMAGE:-ghcr.io/focs-lab/wtc-toolchain:llvm-${REV:0:12}}"

docker image inspect "$IMG" >/dev/null 2>&1 \
  || { echo "образ $IMG не собран, см. docs/toolchain.md" >&2; exit 1; }

WORK=$(mktemp -d)
# A container job runs as root, which is faithful and leaves root-owned files
# behind. Only a container can clear them again.
cleanup() {
  docker run --rm -v "$WORK:/w" "$IMG" rm -rf /w/src /w/steps >/dev/null 2>&1 || true
  rmdir "$WORK" 2>/dev/null || true
}
trap cleanup EXIT
mkdir -p "$WORK/src" "$WORK/steps"
git archive HEAD | tar -x -C "$WORK/src"

python3 - "$WORK" "$REV" "$IMG" <<'PY'
import os, subprocess, sys, yaml

work, rev, img = sys.argv[1], sys.argv[2], sys.argv[3]
job = yaml.safe_load(open(".github/workflows/ci.yml"))["jobs"]["build"]

# The only expressions the build job consumes. Anything else is a hard error
# rather than an empty string, because silently emptying a value is the whole
# class of bug this script exists to catch.
CTX = {
    "needs.toolchain.outputs.rev": rev,
    "needs.toolchain.outputs.image": img,
}

def render(text):
    out, i = [], 0
    while True:
        a = text.find("${{", i)
        if a < 0:
            out.append(text[i:]); break
        b = text.index("}}", a)
        key = text[a + 3:b].strip()
        if key not in CTX:
            sys.exit(f"нет значения для ${{{{ {key} }}}}; допишите его в CTX")
        out.append(text[i:a]); out.append(CTX[key]); i = b + 2
    return "".join(out)

# How GitHub invokes each shell. A container job defaults to `sh`, the runner
# to `bash`; this job is a container job.
SHELLS = {
    None:   ["sh", "-e"],
    "sh":   ["sh", "-e"],
    "bash": ["bash", "--noprofile", "--norc", "-eo", "pipefail"],
}
default_shell = (job.get("defaults", {}).get("run", {}) or {}).get("shell")

failed = 0
for n, step in enumerate(job["steps"]):
    name = step.get("name") or step.get("uses") or f"шаг {n}"
    if "run" not in step:
        print(f"  \033[90m=\033[0m {name} (пропуск: не run-шаг)")
        continue
    shell = step.get("shell", default_shell)
    if shell not in SHELLS:
        sys.exit(f"оболочка {shell!r} не описана в SHELLS")
    path = os.path.join(work, "steps", f"{n}.sh")
    open(path, "w").write(render(step["run"]))
    env = []
    for k, v in (step.get("env") or {}).items():
        env += ["-e", f"{k}={render(str(v))}"]
    cmd = ["docker", "run", "--rm",
           "-v", f"{work}/src:/src", "-v", f"{work}/steps:/steps:ro",
           "-w", "/src", "--cpuset-cpus", os.environ.get("WTC_CPUS", "0-3,28-43"),
           *env, img, *SHELLS[shell], f"/steps/{n}.sh"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    mark = "\033[32m+\033[0m" if r.returncode == 0 else "\033[31mx\033[0m"
    print(f"  {mark} {name}  [{' '.join(SHELLS[shell])}]")
    if r.returncode != 0:
        failed = 1
        for line in (r.stdout + r.stderr).strip().splitlines()[-25:]:
            print(f"        {line}")
        break

sys.exit(failed)
PY
