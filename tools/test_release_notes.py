#!/usr/bin/env python3
"""Regression guard for release_notes.py.

Pins the section boundaries (a release's notes stop at the next `## `
heading, so one release never absorbs the next one's), the optional `v`,
four-component hotfix versions, and that a missing or empty section fails
both modes instead of publishing an empty release body.

Pure Python, no app/Qt. Works on temporary files, never the real CHANGELOG.
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import release_notes  # noqa: E402

SCRIPT = os.path.join(HERE, "release_notes.py")

CHANGELOG = """# Changelog

## [Unreleased]

## [v26.9.10] — 2026-09-30

### Fork

- ten
  continued

## [v26.9.9.1] — 2026-09-20

- hotfix

## [v26.9.9] — 2026-09-17

## [v26.9.8] — 2026-09-13

- eight
"""


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def run(*args, cmake_version="26.9.10"):
    with tempfile.TemporaryDirectory() as tmp:
        changelog = os.path.join(tmp, "CHANGELOG.md")
        cmake = os.path.join(tmp, "CMakeLists.txt")
        with open(changelog, "w", encoding="utf-8") as f:
            f.write(CHANGELOG)
        with open(cmake, "w", encoding="utf-8") as f:
            f.write("cmake_minimum_required(VERSION 3.25)\n"
                    f"project(AetherSDR VERSION {cmake_version} LANGUAGES C CXX)\n")
        return subprocess.run(
            [sys.executable, SCRIPT, *args,
             "--changelog", changelog, "--cmakelists", cmake],
            capture_output=True, text=True)


def main():
    s = release_notes.section

    check(s(CHANGELOG, "v26.9.10") == "### Fork\n\n- ten\n  continued",
          "a section runs to the next `## ` heading, trimmed")
    check(s(CHANGELOG, "26.9.10") == s(CHANGELOG, "v26.9.10"),
          "the leading v is optional")
    check(s(CHANGELOG, "v26.9.9.1") == "- hotfix",
          "four-component hotfix versions resolve")
    check(s(CHANGELOG, "v26.9.9") == "",
          "v26.9.9 is empty, and does not match v26.9.9.1's heading")
    check(s(CHANGELOG, "v26.9") is None,
          "a prefix of a version is not that version")
    check(s(CHANGELOG, "v26.9.7") is None, "a missing section is None")
    check(s(CHANGELOG, "v26.9.8") == "- eight", "the last section runs to EOF")

    check(release_notes.project_version(
              "project(AetherSDR VERSION 26.9.9.1 LANGUAGES C CXX)") == "26.9.9.1",
          "project() version is read, four components included")

    r = run("v26.9.10")
    check(r.returncode == 0 and r.stdout.startswith("### Fork"),
          f"extract prints the section: {r.returncode} {r.stdout!r} {r.stderr!r}")
    check(run("v26.9.9").returncode == 1, "extract fails on an empty section")
    check(run("v26.9.7").returncode == 1, "extract fails on a missing section")

    check(run("--check").returncode == 0, "--check passes when notes exist")
    check(run("--check", cmake_version="26.9.11").returncode == 1,
          "--check fails a version bump with no changelog section")
    check(run("--check", cmake_version="26.9.9").returncode == 1,
          "--check fails a version bump with an empty section")
    check(run().returncode == 2, "neither a version nor --check is a usage error")

    print("release_notes: all checks passed")


if __name__ == "__main__":
    main()
