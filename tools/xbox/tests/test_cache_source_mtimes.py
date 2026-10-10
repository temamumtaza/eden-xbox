#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

from __future__ import annotations

import importlib.util
import os
import pathlib
import subprocess
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).parents[1] / "normalize_cached_source_mtimes.py"
SPEC = importlib.util.spec_from_file_location("normalize_cached_source_mtimes", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
NORMALIZER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(NORMALIZER)


def run_git(repo: pathlib.Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", os.fspath(repo), *args], text=True).strip()


class CachedSourceMtimeTests(unittest.TestCase):
    def test_only_unchanged_tracked_files_are_aged(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = pathlib.Path(directory)
            run_git(repo, "init", "--quiet")
            run_git(repo, "config", "user.name", "Test")
            run_git(repo, "config", "user.email", "test@example.invalid")

            unchanged = repo / "unchanged.cpp"
            changed = repo / "changed.cpp"
            unchanged.write_text("int unchanged = 1;\n", encoding="utf-8")
            changed.write_text("int changed = 1;\n", encoding="utf-8")
            run_git(repo, "add", ".")
            run_git(repo, "commit", "--quiet", "-m", "cached source")
            base = run_git(repo, "rev-parse", "HEAD")

            changed.write_text("int changed = 2;\n", encoding="utf-8")
            added = repo / "added.cpp"
            added.write_text("int added = 3;\n", encoding="utf-8")
            run_git(repo, "add", ".")
            run_git(repo, "commit", "--quiet", "-m", "new source")
            head = run_git(repo, "rev-parse", "HEAD")

            normalized, changed_count, skipped_submodules = NORMALIZER.normalize_repo(
                repo, base, head
            )

            self.assertEqual(normalized, 1)
            self.assertEqual(changed_count, 2)
            self.assertEqual(skipped_submodules, 0)
            self.assertEqual(unchanged.stat().st_mtime_ns, NORMALIZER.OLD_SOURCE_MTIME_NS)
            self.assertGreater(changed.stat().st_mtime_ns, NORMALIZER.OLD_SOURCE_MTIME_NS)
            self.assertGreater(added.stat().st_mtime_ns, NORMALIZER.OLD_SOURCE_MTIME_NS)
            self.assertEqual(run_git(repo, "status", "--porcelain"), "")

    def test_unchanged_submodule_sources_are_aged_too(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            subrepo = root / "subrepo"
            repo = root / "repo"
            subrepo.mkdir()
            repo.mkdir()

            run_git(subrepo, "init", "--quiet")
            run_git(subrepo, "config", "user.name", "Test")
            run_git(subrepo, "config", "user.email", "test@example.invalid")
            sub_source = subrepo / "library.cpp"
            sub_source.write_text("int library = 1;\n", encoding="utf-8")
            run_git(subrepo, "add", "library.cpp")
            run_git(subrepo, "commit", "--quiet", "-m", "submodule source")

            run_git(repo, "init", "--quiet")
            run_git(repo, "config", "user.name", "Test")
            run_git(repo, "config", "user.email", "test@example.invalid")
            parent_source = repo / "parent.cpp"
            parent_source.write_text("int parent = 1;\n", encoding="utf-8")
            run_git(repo, "add", "parent.cpp")
            run_git(repo, "commit", "--quiet", "-m", "parent source")
            subprocess.check_call(
                [
                    "git",
                    "-C",
                    os.fspath(repo),
                    "-c",
                    "protocol.file.allow=always",
                    "submodule",
                    "add",
                    os.fspath(subrepo),
                    "externals/library",
                ],
                stdout=subprocess.DEVNULL,
            )
            run_git(repo, "add", ".")
            run_git(repo, "commit", "--quiet", "-m", "add submodule")
            base = run_git(repo, "rev-parse", "HEAD")

            parent_source.write_text("int parent = 2;\n", encoding="utf-8")
            run_git(repo, "add", "parent.cpp")
            run_git(repo, "commit", "--quiet", "-m", "change parent")
            head = run_git(repo, "rev-parse", "HEAD")

            normalized, changed_count, skipped_submodules = NORMALIZER.normalize_repo(
                repo, base, head
            )

            self.assertGreaterEqual(normalized, 1)
            self.assertEqual(changed_count, 1)
            self.assertEqual(skipped_submodules, 0)
            self.assertEqual(
                (repo / "externals/library/library.cpp").stat().st_mtime_ns,
                NORMALIZER.OLD_SOURCE_MTIME_NS,
            )
            self.assertEqual(run_git(repo, "status", "--porcelain"), "")

    def test_unavailable_cached_revision_is_safe_to_skip(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            repo = pathlib.Path(directory)
            run_git(repo, "init", "--quiet")
            source = repo / "source.cpp"
            source.write_text("int source = 1;\n", encoding="utf-8")
            run_git(repo, "add", "source.cpp")
            run_git(repo, "config", "user.name", "Test")
            run_git(repo, "config", "user.email", "test@example.invalid")
            run_git(repo, "commit", "--quiet", "-m", "source")
            head = run_git(repo, "rev-parse", "HEAD")
            before = source.stat().st_mtime_ns

            self.assertFalse(NORMALIZER.has_commit(repo, "0" * 40))
            with self.assertRaises(ValueError):
                NORMALIZER.normalize_repo(repo, "0" * 40, head)
            self.assertEqual(source.stat().st_mtime_ns, before)


if __name__ == "__main__":
    unittest.main()
