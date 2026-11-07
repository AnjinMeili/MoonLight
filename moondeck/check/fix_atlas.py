#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6", "jsonschema>=4", "markdown>=3"]
# ///
# check_atlas's dependencies: the fixes are its findings, and it brings the graph up to date first.
"""Point MoonAtlas's moved citations at the lines they moved to.

A citation has moved when the text it cites is unchanged but sits at other lines, and check_atlas names those lines as the fix on its finding. This writes them into the claims files and touches nothing else: only a claim whose every other citation is current, only the cite strings inside that claim's own entry, and each claims file whole or not at all, so every other line keeps the commit blame gives it. A claim whose cited text changed is a person's to re-read, and stays as it is.

A rewritten claim is being written until its file is committed, so check_atlas leaves it out until then; from the commit on, it is verified against the text it cites, which is the text it was verified against before.

In the tree it writes nothing but the claims files, beside the graph gen_atlas refreshes under build/, and it does not commit. Its command line, exit codes and the MCP tool serve_atlas lists are read from its declaration, TOOLS.
"""

import contextlib
import json
import os
import re
import shutil
import sqlite3
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "docs"))
import _verbs  # noqa: E402
import check_atlas  # noqa: E402
import gen_atlas  # noqa: E402


def plan(findings: list[dict], match: re.Pattern | None) -> dict:
    """The fixes of each `citation moved` finding, by claims file, keeping those whose `file#claim` matches."""
    by_file: dict[str, list[dict]] = {}
    for f in findings:
        if f["rule"] != "citation moved" or "file" not in f:
            continue
        for fix in f.get("fixes", []):
            if match is None or match.search(f"{f['file']}#{fix['claim']}"):
                by_file.setdefault(f["file"], []).append(fix)
    return by_file


def rewrite(text: str, fixes: list[dict]) -> tuple[str | None, str]:
    """`text` with each fix applied inside its claim's own entry, or None and why when one does not apply exactly once.

    Every fix is found in the text as it was before any is applied, then all are applied, so two cites of one claim that move past each other, `1-2` to `3-4` and `3-4` to `5-6`, each find their own."""
    lines = check_atlas.lines_of(text)
    spans: dict[str, list[tuple[int, int]]] = {}
    for id_, start, end in check_atlas.entry_spans(lines):
        spans.setdefault(id_, []).append((start, end))
    edits: dict[int, list[tuple[int, str, str]]] = {}   # line -> (column, old, new), all found before any is made
    for fix in fixes:
        at = spans.get(fix["claim"], [])
        if len(at) != 1:
            return None, f"{fix['claim']} is {'not' if not at else 'more than once'} in the file"
        start, end = at[0]
        old, new = json.dumps(fix["from"]), json.dumps(fix["to"])   # a cite as the file spells it, quotes included, so `a.h:1` never matches inside `a.h:12`
        found = sum(lines[i].count(old) for i in range(start, end + 1))
        if found != 1:
            return None, f"{fix['claim']} cites {fix['from']} {found or 'no'} time{'' if found == 1 else 's'} in its entry, not once"
        line = next(i for i in range(start, end + 1) if old in lines[i])
        edits.setdefault(line, []).append((lines[line].index(old), old, new))
    for line, at in edits.items():
        for column, old, new in sorted(at, reverse=True):   # right to left, so an edit leaves the columns before it where they were
            lines[line] = lines[line][:column] + new + lines[line][column + len(old):]
    return "\n".join(lines) + ("\n" if text.endswith("\n") else ""), ""


def write(path: Path, before: str, after: str) -> str:
    """Replace `path` with `after` in one step, when it still holds `before`."""
    if path.read_text(encoding="utf-8") != before:
        return "changed while the fixes were planned"
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="") as out:
            out.write(after)
        shutil.copymode(path, tmp)   # mkstemp makes the file 0600, and the replace would hand the claims file that mode
        os.replace(tmp, path)
    except OSError as e:
        return f"could not be written: {e}"
    finally:
        Path(tmp).unlink(missing_ok=True)   # gone already once replaced; left behind by any other end, an interrupt included
    return ""


def apply(findings: list[dict], match: re.Pattern | None, dry_run: bool) -> list[dict]:
    """Each claims file with its fixes, whether they apply, and, without `dry_run`, whether it was written."""
    report = []
    for file, fixes in sorted(plan(findings, match).items()):
        path = check_atlas.ROOT / file
        try:
            before = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError) as e:   # strict, since a fix written through a replaced byte would change more than its cite
            report.append({"file": file, "fixes": fixes, "problem": f"could not be read: {e}"})
            continue
        after, problem = rewrite(before, fixes)
        if after is not None and not dry_run:
            problem = write(path, before, after)
        report.append({"file": file, "fixes": fixes, "problem": problem})
    return report


LIMIT = 20


def fix(dry_run=False, match=None, limit=LIMIT) -> dict:
    """Point each moved citation at the lines its text moved to, in the claims files alone, each file whole or not at all; never commits."""
    try:
        pattern = re.compile(match) if match else None
    except re.error as e:
        raise _verbs.Refused(f"--match is not a regular expression: {e}") from e
    print(gen_atlas.refresh(), file=sys.stderr)
    with contextlib.closing(sqlite3.connect(f"{gen_atlas.DB.as_uri()}?mode=ro", uri=True)) as con:
        findings = check_atlas.stale(con)
    return {"dry_run": dry_run, "limit": limit, "files": apply(findings, pattern, dry_run)}


def text(a: dict) -> list[str]:
    report, dry_run, limit = a["files"], a["dry_run"], a["limit"]
    done = [r for r in report if not r["problem"]]
    fixes = [(r["file"], fx) for r in done for fx in r["fixes"]]
    claims = {(file, fx["claim"]) for file, fx in fixes}
    verb = "would rewrite" if dry_run else "rewrote"
    lines = [f"fix_atlas: {verb} {len(fixes)} citation(s) in {len(claims)} claim(s) across {len(done)} file(s)"
             + (f"; {len(report) - len(done)} file(s) left as they were" if len(done) < len(report) else "")]
    lines += [f"  not applied: {r['file']}: {r['problem']}" for r in report if r["problem"]]
    lines += [f"  {file}#{fx['claim']}: {fx['from']} -> {fx['to']}" for file, fx in fixes[:limit]]
    if len(fixes) > limit:
        lines.append(f"  showing {limit} of {len(fixes)}; more: --limit {len(fixes)}")
    if fixes:
        lines.append("Next: run again without --dry-run to write them." if dry_run else
                     "Next: read the diff and commit the claims files; check_atlas leaves a rewritten claim out until then.")
    return lines


TOOLS = [{
    "name": "fix", "run": fix, "text": text, "description": fix.__doc__,
    "params": {"dry_run": {"type": "boolean", "default": False, "description": "say what would change and change nothing"},
               "match": {"type": "string", "description": "only claims whose `file#id` matches this regular expression, such as `pilot/` or `#core-04`"},
               "limit": {"type": "integer", "minimum": 0, "default": LIMIT, "description": "list at most this many rewritten citations; the totals count all, and --json lists all"}},
    "annotations": {"readOnlyHint": False, "destructiveHint": True, "idempotentHint": True, "openWorldHint": False},
    "reads": ["check_atlas's moved findings", "the claims files"],
    "writes": ["the claims files under docs/work/, unless dry_run", "build/atlas/atlas.sqlite, rebuilt when the tree changed"],
    "examples": ["--dry-run", "--match core- --dry-run", ""],
    "exits": {0: "every fix written, or with --dry-run every fix applies, or nothing to fix", 1: "a fix that does not apply, its whole file left as it was", 2: "a usage error"},
    "next": ["check"],
    "code": lambda a: 1 if any(r["problem"] for r in a["files"]) else 0,
}]
SCRIPT = "uv run moondeck/check/fix_atlas.py"


def main(argv=None) -> int:
    return _verbs.main(SCRIPT, __doc__, TOOLS, "atlas_", argv=argv)


if __name__ == "__main__":
    _verbs.script(SCRIPT, __doc__, TOOLS, "atlas_")
