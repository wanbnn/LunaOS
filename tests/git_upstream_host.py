#!/usr/bin/env python3
"""Host-only compatibility smoke test for the *pinned upstream* Git source.

This does NOT demonstrate Git running in LunaOS. It is a comparison fixture
for the eventual guest port. No internet, credentials or external remotes.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(git: Path, *args: str, cwd: Path | None = None, extra: dict[str, str] | None = None) -> str:
    env = os.environ.copy()
    env.update({
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_CONFIG_GLOBAL": os.devnull,
        "GIT_TERMINAL_PROMPT": "0",
        "GIT_AUTHOR_NAME": "Luna Git CI",
        "GIT_AUTHOR_EMAIL": "git-ci@invalid.example",
        "GIT_COMMITTER_NAME": "Luna Git CI",
        "GIT_COMMITTER_EMAIL": "git-ci@invalid.example",
    })
    env.update(extra or {})
    command = [str(git), *args]
    proc = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True)
    if proc.returncode:
        raise AssertionError(
            f"{command!r} failed ({proc.returncode})\n{proc.stdout}\n{proc.stderr}"
        )
    return proc.stdout.strip()


def verify(git: Path) -> None:
    version = run(git, "--version")
    if not version.startswith("git version 2.56.1"):
        raise AssertionError(f"Wrong upstream Git version: {version}")
    with tempfile.TemporaryDirectory(prefix="luna-upstream-git-") as t:
        base = Path(t)
        origin, alice, bob = base / "origin.git", base / "alice", base / "bob"
        run(git, "init", "--bare", str(origin))
        run(git, "init", "-b", "main", str(alice))
        (alice / "README.md").write_text("LunaOS full Git compatibility baseline\n", encoding="utf-8")
        payload = bytes(range(256)) * 4096
        (alice / "binary.dat").write_bytes(payload)
        run(git, "add", "-A", cwd=alice)
        run(git, "commit", "-m", "initial binary and text", cwd=alice)
        first_commit = run(git, "rev-parse", "HEAD", cwd=alice)
        if len(first_commit) != 40:
            raise AssertionError("Commit ID is not SHA-1 length")
        expected_hash = hashlib.sha1(b"blob " + str(len(payload)).encode() + b"\0" + payload).hexdigest()
        if run(git, "hash-object", "binary.dat", cwd=alice) != expected_hash:
            raise AssertionError("Git blob SHA-1 differs from Git object format")
        run(git, "remote", "add", "origin", str(origin), cwd=alice)
        run(git, "push", "-u", "origin", "main", cwd=alice)
        run(git, "clone", "--branch", "main", str(origin), str(bob))
        if (bob / "binary.dat").read_bytes() != payload:
            raise AssertionError("Clone changed binary content")
        run(git, "switch", "-c", "feature", cwd=bob)
        (bob / "feature.txt").write_text("feature from Bob\n", encoding="utf-8")
        run(git, "add", "feature.txt", cwd=bob)
        run(git, "commit", "-m", "feature branch", cwd=bob)
        run(git, "push", "-u", "origin", "feature", cwd=bob)
        run(git, "fetch", "--prune", "origin", cwd=alice)
        run(git, "merge", "--no-ff", "-m", "merge feature", "origin/feature", cwd=alice)
        run(git, "push", "origin", "main", cwd=alice)
        run(git, "switch", "main", cwd=bob)
        run(git, "pull", "--ff-only", "origin", "main", cwd=bob)
        if not (bob / "feature.txt").exists():
            raise AssertionError("Fetch/merge/pull lost feature")
        run(git, "switch", "-c", "topic", cwd=bob)
        (bob / "topic.txt").write_text("topic\n", encoding="utf-8")
        run(git, "add", "topic.txt", cwd=bob)
        run(git, "commit", "-m", "topic commit", cwd=bob)
        run(git, "rebase", "main", cwd=bob)
        run(git, "switch", "main", cwd=bob)
        (bob / "README.md").write_text("temporary uncommitted edit\n", encoding="utf-8")
        run(git, "stash", "push", "-m", "temporary", cwd=bob)
        run(git, "stash", "pop", cwd=bob)
        if (bob / "README.md").read_text(encoding="utf-8") != "temporary uncommitted edit\n":
            raise AssertionError("stash/pop failed to restore working tree")
        run(git, "restore", "README.md", cwd=bob)
        run(git, "fsck", "--full", cwd=alice)
        run(git, "gc", cwd=alice)
        if run(git, "status", "--porcelain", cwd=alice):
            raise AssertionError("Clean repo reported changed")
        if not run(git, "log", "--oneline", "-3", cwd=alice):
            raise AssertionError("History is empty")
    print("PASS: upstream Git 2.56.1 local init/add/commit/branch/merge/rebase/stash/clone/fetch/pull/push/fsck/gc")
    print("NOTE: host-only local-remotes; LunaOS guest execution and HTTPS/SSH still untested.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--git", type=Path, default=Path("third_party/git/git"))
    args = parser.parse_args()
    if not args.git.is_file():
        parser.error(f"Git binary does not exist: {args.git}. Build the upstream source first.")
    verify(args.git.resolve())
    return 0


if __name__ == "__main__":
    sys.exit(main())
