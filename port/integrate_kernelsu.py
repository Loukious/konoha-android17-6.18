#!/usr/bin/env python3
"""Fetch KernelSU Next dev at build time; materialize and verify built-in sources."""

import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile

HERE = Path(__file__).resolve().parent
UPSTREAM = "https://github.com/KernelSU-Next/KernelSU-Next.git"
BRANCH = "dev"
COMMON_REVISION = json.loads((HERE / "sources.json").read_text())["common"]["revision"]


def expected(common):
    source = common.parent / ".kernelsu-next"
    pin = json.loads((source / "source.json").read_text())
    data = (source / "source.tar").read_bytes()
    if hashlib.sha256(data).hexdigest() != pin["sha256"]:
        raise ValueError("KernelSU source archive hash mismatch")
    files = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:") as archive:
        for entry in archive:
            if not entry.isfile():
                continue
            path = Path(entry.name)
            if path.is_absolute() or ".." in path.parts:
                raise ValueError("unsafe KernelSU archive path")
            if path.parts[0] == "uapi":
                # Upstream kernel/include/uapi is a symlink to ../../uapi.
                # Materialize its target so Kleaf's source glob sees the headers.
                target = Path("drivers/kernelsu/include") / path
            else:
                target = Path("drivers/kernelsu") / (path.relative_to("kernel")
                         if path.parts[0] == "kernel" else path)
            files[target.as_posix()] = archive.extractfile(entry).read()
    kbuild = files["drivers/kernelsu/Kbuild"].decode()
    start = kbuild.index("LPATH :=")
    end = kbuild.index("KSU_NEW_DCACHE_FLUSH :=")
    # Resolve Git metadata before entering Kleaf's hermetic build sandbox.
    # This is upstream's 30000 + rev-list --count algorithm on the fetched dev HEAD.
    kbuild = (kbuild[:start] + f"# Upstream dev {pin['revision']}; see artifact inputs/kernelsu-next.\n"
              + f"ccflags-y += -DKSU_VERSION={pin['version']}\n"
              + 'ccflags-y += -DKSU_VERSION_TAG=\\"' + pin['tag'] + '\\"\n\n'
              + kbuild[end:])
    files["drivers/kernelsu/Kbuild"] = kbuild.encode()
    additions = {
        "drivers/Makefile": '\nobj-$(CONFIG_KSU) += kernelsu/\n',
        "drivers/Kconfig": '\nsource "drivers/kernelsu/Kconfig"\n',
    }
    for name, addition in additions.items():
        original = subprocess.check_output([
            "git", "-C", str(common), "show", f"{COMMON_REVISION}:{name}"])
        files[name] = original + addition.encode()
    return files, additions


def verify(common):
    if (common / "drivers/kernelsu").is_symlink():
        raise ValueError("unexpected KernelSU source symlink")
    files, additions = expected(common)
    changed = subprocess.check_output([
        "git", "-C", str(common), "diff", "HEAD", "--name-only"], text=True).splitlines()
    if set(changed) != set(additions):
        raise ValueError("unexpected tracked changes in common")
    actual = {p.relative_to(common).as_posix() for p in
              (common / "drivers/kernelsu").rglob("*") if p.is_file()}
    wanted = {p for p in files if p.startswith("drivers/kernelsu/")}
    if actual != wanted:
        raise ValueError("KernelSU source file set mismatch")
    for name, contents in files.items():
        path = common / name
        if path.is_symlink() or path.read_bytes() != contents:
            raise ValueError(f"KernelSU integration differs: {name}")
    return files


def integrate(common):
    previous = None
    if (common / "drivers/kernelsu").exists():
        previous = verify(common)
    elif subprocess.check_output([
            "git", "-C", str(common), "status", "--porcelain", "-uno"]):
        raise ValueError("preserve tracked common changes before integrating KernelSU")
    source = common.parent / ".kernelsu-next"
    repo = source / "repo"
    source.mkdir(exist_ok=True)
    if repo.exists():
        if subprocess.check_output(["git", "-C", str(repo), "status", "--porcelain"]):
            raise ValueError("preserve local changes in the KernelSU Next checkout")
        remote = subprocess.check_output([
            "git", "-C", str(repo), "remote", "get-url", "origin"], text=True).strip()
        if remote != UPSTREAM:
            raise ValueError("unexpected KernelSU Next origin")
        subprocess.run(["git", "-C", str(repo), "fetch", "origin", BRANCH, "--tags"], check=True)
    else:
        subprocess.run(["git", "clone", "--branch", BRANCH, UPSTREAM, str(repo)], check=True)
    git = lambda *args: subprocess.check_output(["git", "-C", str(repo), *args], text=True).strip()
    revision = git("rev-parse", f"origin/{BRANCH}")
    subprocess.run(["git", "-C", str(repo), "checkout", "--detach", revision], check=True)
    count = int(git("rev-list", "--count", revision))
    tag = git("describe", "--tags", "--abbrev=0", revision)
    data = subprocess.check_output([
        "git", "-C", str(repo), "archive", "--format=tar", revision, "kernel", "uapi", "LICENSE"])
    pin = {"repository": UPSTREAM, "branch": BRANCH, "revision": revision,
           "tag": tag, "git_commit_count": count, "version": 30000 + count,
           "sha256": hashlib.sha256(data).hexdigest()}
    # Refresh only a previously verified source tree. Unknown edits are never reset.
    if previous is not None:
        verify(common)
        shutil.rmtree(common / "drivers/kernelsu")
    (source / "source.tar").write_bytes(data)
    (source / "source.json").write_text(json.dumps(pin, indent=2) + "\n")
    files, _ = expected(common)
    for name, contents in files.items():
        path = common / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
    verify(common)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("workspace", type=Path)
    parser.add_argument("--verify-only", action="store_true")
    args = parser.parse_args()
    (verify if args.verify_only else integrate)(args.workspace.resolve() / "common")
    pin = json.loads((args.workspace / ".kernelsu-next/source.json").read_text())
    print(f"KernelSU Next dev {pin['revision']} / {pin['version']}: source and integration verified")
