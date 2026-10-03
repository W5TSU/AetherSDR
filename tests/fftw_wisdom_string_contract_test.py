#!/usr/bin/env python3
"""No fftw(f)_export_wisdom_to_string() anywhere in src/.

It returns a string FFTW allocated with malloc() in its OWN C runtime. The
Windows build ships the official MinGW FFTW DLL (msvcrt.dll) while AetherSDR
is MSVC/UCRT, so the string can be freed correctly by neither fftw_free() (an
aligned free for fftw_malloc blocks) nor the app's free() (another heap).
Freeing it with fftw_free() corrupted the heap on every Windows HL2 connect in
26.10.1/26.10.2 (0xc0000374 at "DSP setup: opening receive chain(s)"), while
Linux, where all three are glibc's, never showed it.

Use fftw_export_wisdom(write_char, data) / fftw_export_wisdom_to_filename()
instead: nothing allocated by FFTW crosses the DLL boundary. This guard exists
because no Linux test can reproduce the cross-CRT free itself.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parent.parent
PATTERN = re.compile(r"\bfftwf?l?_export_wisdom_to_string\s*\(")

offenders = []
for path in sorted((ROOT / "src").rglob("*")):
    if path.suffix not in {".cpp", ".cc", ".c", ".h", ".hpp"}:
        continue
    for lineno, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        code = line.split("//", 1)[0]
        if PATTERN.search(code):
            offenders.append(f"{path.relative_to(ROOT)}:{lineno}: {line.strip()}")

if offenders:
    print("FAIL: fftw_export_wisdom_to_string() used; see this test's docstring for why:")
    print("\n".join(offenders))
    sys.exit(1)
print("OK: no fftw_export_wisdom_to_string() in src/")
