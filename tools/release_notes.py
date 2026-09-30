#!/usr/bin/env python3
"""
Release notes, from CHANGELOG.md.

Every GitHub release used to say only "Release vX": the release workflows
attach binaries and nothing ever wrote a body, so the notes lived only in
CHANGELOG.md. The changelog section written at release prep IS the release
notes, so this tool extracts it rather than asking anyone to write them twice.

Two modes:

    python tools/release_notes.py v26.9.10
        Print the body of the `## [v26.9.10] — <date>` section (the lines up
        to the next `## ` heading), trimmed. Exit 1 when the section is missing
        or empty. release-notes.yml publishes this as the release body.

    python tools/release_notes.py --check
        Exit 1 unless CHANGELOG.md has a non-empty section for the version in
        CMakeLists.txt's project(). The version only changes at release prep,
        so this is the gate that makes a bump without notes fail in PR CI
        instead of shipping a release with an empty body.

The leading `v` is optional in both. Stdlib only.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CHANGELOG = REPO / "CHANGELOG.md"
CMAKELISTS = REPO / "CMakeLists.txt"

_PROJECT_VERSION = re.compile(r"^project\(AetherSDR\s+VERSION\s+([0-9][0-9.]*)\b",
                              re.MULTILINE)


def section(changelog_text: str, version: str) -> str | None:
    """Body of the `## [v<version>]` section, trimmed; None if no such heading."""
    version = version.removeprefix("v")
    heading = re.compile(r"^## \[v?" + re.escape(version) + r"\](\s|$)")
    lines = changelog_text.splitlines()
    for i, line in enumerate(lines):
        if heading.match(line):
            body = []
            for following in lines[i + 1:]:
                if following.startswith("## "):
                    break
                body.append(following)
            return "\n".join(body).strip("\n")
    return None


def project_version(cmake_text: str) -> str | None:
    match = _PROJECT_VERSION.search(cmake_text)
    return match.group(1) if match else None


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Extract or check the CHANGELOG.md section for a release")
    parser.add_argument("version", nargs="?",
                        help="release version or tag, e.g. v26.9.10")
    parser.add_argument("--check", action="store_true",
                        help="require notes for CMakeLists.txt's project version")
    parser.add_argument("--changelog", type=Path, default=CHANGELOG)
    parser.add_argument("--cmakelists", type=Path, default=CMAKELISTS)
    args = parser.parse_args()

    if args.check == bool(args.version):
        parser.error("give a version, or --check, but not both")

    version = args.version
    if args.check:
        version = project_version(args.cmakelists.read_text(encoding="utf-8"))
        if version is None:
            print(f"release-notes: no project(AetherSDR VERSION ...) in {args.cmakelists}",
                  file=sys.stderr)
            return 1

    body = section(args.changelog.read_text(encoding="utf-8"), version)
    if body is None:
        print(f"release-notes: CHANGELOG.md has no `## [v{version.removeprefix('v')}]` "
              "section. Write one at release prep (see AGENTS.md, "
              "'Version and release files'); it becomes the GitHub release notes.",
              file=sys.stderr)
        return 1
    if not body.strip():
        print(f"release-notes: the CHANGELOG.md section for v{version.removeprefix('v')} "
              "is empty; it becomes the GitHub release notes, so it needs content.",
              file=sys.stderr)
        return 1

    if args.check:
        print(f"release-notes: OK — CHANGELOG.md has notes for v{version}")
    else:
        print(body)
    return 0


if __name__ == "__main__":
    sys.exit(main())
