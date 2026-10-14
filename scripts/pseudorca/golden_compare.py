"""Compare two golden_cases.py outputs: baseline vs candidate.

    python3 golden_compare.py <baseline-dir> <candidate-dir> [--allow-new-cases] [--only=case,case]

Exit status 0 only when every baseline case is identical in the candidate. --help-json may gain options (new SLA
settings) but no baseline option may disappear or change. Where G-code differs, the first differing lines are shown.
"""

from __future__ import annotations

import difflib
import gzip
import json
import sys
from pathlib import Path


def load(folder: Path):
    return json.loads((folder / "summary.json").read_text())


def gcode_diff(base: Path, cand: Path, case: str, name: str) -> list[str]:
    a, b = base / case / f"{name}.gz", cand / case / f"{name}.gz"
    if not (a.exists() and b.exists()):
        return []
    la = gzip.open(a, "rt", errors="replace").read().splitlines()
    lb = gzip.open(b, "rt", errors="replace").read().splitlines()
    diff = list(difflib.unified_diff(la, lb, "baseline", "candidate", n=1, lineterm=""))
    return diff[:24] + (["    ... (%d more diff lines)" % (len(diff) - 24)] if len(diff) > 24 else [])


def compare_case(case: str, a: dict, b: dict, base: Path, cand: Path) -> list[str]:
    problems: list[str] = []
    for key in sorted(set(a) | set(b)):
        if key == "files":
            continue
        if a.get(key) != b.get(key):
            problems.append(f"{key}: baseline={json.dumps(a.get(key))[:300]} candidate={json.dumps(b.get(key))[:300]}")
    fa, fb = a.get("files", {}), b.get("files", {})
    for name in sorted(set(fa) | set(fb)):
        if fa.get(name) == fb.get(name):
            continue
        problems.append(f"file {name}: {'missing in candidate' if name not in fb else 'missing in baseline' if name not in fa else 'differs'}")
        problems += ["    " + line for line in gcode_diff(base, cand, case, name)]
    return problems


def compare_help(base: Path, cand: Path) -> list[str]:
    a = {i["key"]: i for i in json.loads((base / "help.json").read_text())}
    b = {i["key"]: i for i in json.loads((cand / "help.json").read_text())}
    problems = [f"--help-json lost option {k}" for k in sorted(set(a) - set(b))]
    problems += [f"--help-json option {k} changed: {a[k]} -> {b[k]}" for k in sorted(set(a) & set(b)) if a[k] != b[k]]
    added = sorted(set(b) - set(a))
    if added:
        print(f"note: --help-json gained {len(added)} option(s): {', '.join(added[:12])}{' ...' if len(added) > 12 else ''}")
    return problems


def main() -> int:
    base, cand = Path(sys.argv[1]), Path(sys.argv[2])
    allow_new = "--allow-new-cases" in sys.argv
    only = next((arg.split("=", 1)[1].split(",") for arg in sys.argv if arg.startswith("--only=")), None)
    a, b = load(base), load(cand)
    if only:
        a = {k: v for k, v in a.items() if k in only}
        b = {k: v for k, v in b.items() if k in only}
    failures = 0
    for case in sorted(a):
        if case == "help_json":
            continue
        if case not in b:
            print(f"FAIL {case}: missing in candidate")
            failures += 1
            continue
        problems = compare_case(case, a[case], b[case], base, cand)
        print(f"{'FAIL' if problems else 'ok  '} {case}")
        for line in problems:
            print("     " + line)
        failures += bool(problems)
    for case in sorted(set(b) - set(a)):
        print(f"{'note' if allow_new else 'FAIL'} {case}: not in the baseline")
        failures += not allow_new
    if "help_json" in a and "help_json" in b and not only:
        problems = compare_help(base, cand)
        print(f"{'FAIL' if problems else 'ok  '} help_json")
        for line in problems:
            print("     " + line)
        failures += bool(problems)
    print(f"\n{len(a)} baseline case(s), {failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
