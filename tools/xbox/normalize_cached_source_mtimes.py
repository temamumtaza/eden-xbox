#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

"""Keep restored Ninja outputs reusable after actions/checkout refreshes source timestamps.

The cache key ends in the source revision that produced the cached build tree. Files unchanged
between that revision and the checkout can safely be older than the cached objects; files changed
in the checkout retain their fresh timestamps so Ninja rebuilds their dependents.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


OLD_SOURCE_MTIME_NS = 946684800_000_000_000  # 2000-01-01 UTC


def git_bytes(repo: Path, *args: str) -> bytes:
    result = subprocess.run(
        ["git", "-C", os.fspath(repo), *args],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return result.stdout


def has_commit(repo: Path, commit: str) -> bool:
    return subprocess.run(
        ["git", "-C", os.fspath(repo), "cat-file", "-e", f"{commit}^{{commit}}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    ).returncode == 0


def direct_gitlinks(repo: Path, commit: str) -> dict[bytes, str]:
    links: dict[bytes, str] = {}

    def visit_tree(tree: str, prefix: bytes = b"") -> None:
        entries = git_bytes(repo, "ls-tree", "-z", tree).split(b"\0")
        for entry in entries:
            if not entry:
                continue
            metadata, path = entry.split(b"\t", 1)
            mode, object_type, object_id = metadata.split(b" ", 2)
            full_path = prefix + path
            if mode == b"160000" and object_type == b"commit":
                links[full_path] = object_id.decode("ascii")
            elif object_type == b"tree":
                visit_tree(object_id.decode("ascii"), full_path + b"/")

    visit_tree(commit)
    return links


def normalize_repo(repo: Path, base_commit: str, head_commit: str) -> tuple[int, int, int]:
    """Normalize unchanged tracked files and recurse into unchanged/changed submodules.

    Returns (normalized file count, changed file count, skipped submodule count).
    """
    if not has_commit(repo, base_commit) or not has_commit(repo, head_commit):
        raise ValueError(f"source revision is unavailable in {repo}")

    changed = {
        item
        for item in git_bytes(
            repo, "diff", "--name-only", "--no-renames", "-z", base_commit, head_commit
        ).split(b"\0")
        if item
    }
    normalized = 0
    tracked = git_bytes(repo, "ls-files", "-z").split(b"\0")
    for item in tracked:
        if not item or item in changed:
            continue
        path = repo / os.fsdecode(item)
        if not path.is_file() or path.is_symlink():
            continue
        stat = path.stat()
        os.utime(path, ns=(stat.st_atime_ns, OLD_SOURCE_MTIME_NS), follow_symlinks=False)
        normalized += 1

    old_links = direct_gitlinks(repo, base_commit)
    new_links = direct_gitlinks(repo, head_commit)
    changed_submodules = 0
    for path_bytes, new_commit in new_links.items():
        old_commit = old_links.get(path_bytes)
        if old_commit is None:
            changed_submodules += 1
            continue

        submodule = repo / os.fsdecode(path_bytes)
        try:
            current_commit = git_bytes(submodule, "rev-parse", "HEAD").strip().decode("ascii")
            if current_commit != new_commit:
                changed_submodules += 1
                continue
            sub_normalized, sub_changed, sub_skipped = normalize_repo(
                submodule, old_commit, current_commit
            )
        except (OSError, subprocess.CalledProcessError, ValueError):
            # Leave an unavailable submodule's checkout timestamps untouched. That may cause
            # additional compilation, but it cannot hide a source change from Ninja.
            changed_submodules += 1
            continue
        normalized += sub_normalized
        changed_submodules += sub_changed + sub_skipped

    return normalized, len(changed), changed_submodules


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--cached-sha", required=True)
    parser.add_argument("--current-sha", required=True)
    args = parser.parse_args()

    repo = args.repo.resolve()
    try:
        normalized, changed, skipped_submodules = normalize_repo(
            repo, args.cached_sha, args.current_sha
        )
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"Cached source timestamps left unchanged: {error}", file=sys.stderr)
        return 0

    print(
        "Cached Ninja source timestamps: "
        f"normalized {normalized} unchanged files; "
        f"left {changed} changed files fresh; "
        f"skipped/changed submodules {skipped_submodules}."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
