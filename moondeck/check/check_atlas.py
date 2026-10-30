#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6", "jsonschema>=4", "markdown>=3"]
# ///
# gen_atlas's dependencies: the check first brings the graph up to date, and gen_atlas builds it.
"""Verify MoonAtlas: each claim cites what exists and the text it was verified against, each edge lands on a node, each module has its doc.

A claim is a sentence a researcher wrote about the tree as it stood, so it holds while what it cites does. A citation
resolves when its file is in the tree, its anchor is on its page and its lines are within the file. A claim was last
verified in the newer of the commit its own lines last changed in and a review that found it correct, so git holds the
baseline and a claim carries none of its own; research reads a clean tree, so that commit holds the text it read. Each
citation is compared with its text in that commit: unchanged is current, the same text once elsewhere has moved, and
anything else makes the claim stale, a sentence a person re-reads. Section by section for an anchor, line by line for a
range, the whole file for a bare path, so a change elsewhere in a long page leaves a claim current. An edge
whose end no source defines points nowhere, such as a test naming a module that is not one or a review of a claim since
removed; a base class is left out, since a base may live in a library outside the tree.

    uv run moondeck/check/check_atlas.py

Exit codes: 0 every citation resolves, every module is documented and every source was read, with stale claims, moved
citations and dangling edges reported as warnings; 1 otherwise.
"""

import contextlib
import functools
import json
import re
import sqlite3
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))
import _findings  # noqa: E402
import gen_atlas  # noqa: E402
from gen_moonwalk import anchor_lines  # noqa: E402

STANDARD = "docs/contributing/documentation-standards.md#what-we-document"
RULES = {
    "source skipped": {"reason": "A source could not be read as it should, a stage of gen_atlas or git's list of commits for blame to look past, so the other rules read a partial picture.", "fix": "Run gen_atlas and read the stage it names on stderr, or correct `.git-blame-ignore-revs`, whose lines are full 40-character commit ids.", "standard": STANDARD},
    "citation unresolved": {"reason": "A claim cites a file outside the tree, an anchor its page lacks or lines past the file's end, so its reader lands nowhere.", "fix": "Point the citation at the lines or section that hold the fact, or remove the claim when the fact is gone.", "standard": STANDARD},
    "claim stale": {"reason": "Text the claim cites differs from the text it was verified against, so its fact may not hold.", "fix": "Re-read the claim against the cited text: correct its citation or text and commit it, record a review that finds it correct, or remove it.", "standard": STANDARD},
    "citation moved": {"reason": "The text a claim cites is unchanged but sits at other lines, so its reader lands beside it.", "fix": "Change the citation's lines to the ones the finding names, and commit the claims file.", "standard": STANDARD},
    "edge to nothing": {"reason": "A row of the graph points at a node no source defines, such as a test naming a module that is not one.", "fix": "Name what exists: the module, class or file stem the test pins, or the claim the review checked.", "standard": STANDARD},
    "module undocumented": {"reason": "A registered module has no doc section in help.json, so `why` and `impact` have nowhere to send its reader.", "fix": "Give the module's registerType its docPath.", "standard": "docs/contributing/documentation-standards.md#module-pages"},
}

IGNORE_REVS = ".git-blame-ignore-revs"   # git's own list of commits that change no fact, such as a reformat, which blame looks past
HELP = "docs/help/help.json"
_LINES_RE = re.compile(r"(\d+)(?:-(\d+))?")
_ID_LINE_RE = re.compile(r'^\s*(?:\{[^{}]*?)?"id":\s*"([^"]+)"')   # an entry's id, on its own line or anywhere on the entry's one line


def node(con, id_: str):
    return con.execute("SELECT kind, name, path FROM nodes WHERE id = ?", (id_,)).fetchone()


def lines_of(text: str) -> list[str]:
    """A file's lines as git and an editor number them, each ended by a newline alone; `splitlines` also ends one at U+2028 and its kind."""
    return text.removesuffix("\n").split("\n") if text else []


@functools.cache
def text_of(path: str) -> str | None:
    """A tree file's text, or None for a node with no file on disk, a page the docs build generates."""
    try:
        return (ROOT / path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None


def at_claim(path: str, claim: str) -> dict:
    """Where a claim is written: its claims file, and the line of its id there."""
    cid = claim.rpartition("#")[2]
    text = text_of(path) or ""
    at = text.find(f'"id": "{cid}"')
    return {"file": path, "line": text.count("\n", 0, at) + 1} if at >= 0 else {"file": path}


def skipped(con) -> list[dict]:
    """Each source gen_atlas skipped on its last build."""
    row = con.execute("SELECT value FROM meta WHERE key = 'skipped'").fetchone()
    return [{"rule": "source skipped", "message": f"gen_atlas skipped its {stage} stage", "level": "error"}
            for stage in (row[0].split(",") if row and row[0] else [])]


def unresolved(con) -> list[dict]:
    """Each citation whose file is not in the tree, whose anchor its page lacks, or whose lines are not within its file."""
    found = []
    for claim, path, dst, lines in con.execute(
            "SELECT e.src, n.path, e.dst, e.detail FROM edges e JOIN nodes n ON n.id = e.src WHERE e.rel = 'cites' ORDER BY e.src, e.dst"):
        cited = dst.split(":", 1)[1]
        page = cited.partition("#")[0]
        why = None
        if node(con, f"file:{page}") is None:
            why = f"{page} is not in the tree"
        elif node(con, dst) is None:
            why = f"{page} has no #{cited.partition('#')[2]}"
        elif lines and text_of(page) is not None:
            m = _LINES_RE.fullmatch(lines)
            count = len(lines_of(text_of(page)))
            first, last = (int(m[1]), int(m[2] or m[1])) if m else (0, 0)
            if not 1 <= first <= last <= count:
                why = f"{page}:{lines} is not within its {count} lines"
        if why:
            found.append({"rule": "citation unresolved", "message": f"{claim.removeprefix('claim:')} cites {why}", "level": "error", **at_claim(path, claim)})
    return found


class History:
    """What git holds of a claim: the commit each entry of a claims or review file last changed in, the files changed since a commit, and a file's text at a commit."""

    UNCOMMITTED = "0" * 40

    def __init__(self):
        self.root = ROOT
        self.refused: set[str] = set()   # why blame refused the ignore list, read without it

    def git(self, *args) -> str | None:
        done = subprocess.run(["git", *args], cwd=self.root, capture_output=True, text=True, errors="replace")   # a cited file need not be UTF-8
        return done.stdout if done.returncode == 0 else None

    @functools.cached_property
    def order(self) -> dict:
        """Each commit of HEAD's history by its place, 0 the newest."""
        return {c: i for i, c in enumerate((self.git("rev-list", "HEAD") or "").split())}

    @functools.cache
    def entries(self, path: str) -> list[tuple[str, str | None]]:
        """Each entry's id and the newest commit among its lines, in file order: an entry owns the lines from its opening brace to its closing one, and one with a line no commit holds yet has None, being written.

        An entry holds no object of its own, so its first line ending in a brace closes it, and what follows the list, such as a file's notes, is no entry's. A line of punctuation alone holds no fact: appending an entry adds a comma to the closing brace before it, which leaves that entry as verified as it was."""
        lines = lines_of(text_of(path) or "")
        ignore = ["--ignore-revs-file", IGNORE_REVS] if (self.root / IGNORE_REVS).exists() else []
        out = self.git("blame", "--porcelain", "-w", *ignore, "--", path)
        if out is None and ignore and (out := self.git("blame", "--porcelain", "-w", "--", path)) is not None:
            self.refused.add(f"git blame refuses {IGNORE_REVS}, so blame read without it")   # a malformed list would otherwise leave every claim unchecked
        blamed = re.findall(r"^([0-9a-f]{40}) \d+ \d+", out or "", re.MULTILINE)
        if len(blamed) != len(lines):
            blamed = [self.UNCOMMITTED] * len(lines)   # a file git does not track is being written
        out = []
        for at, id_ in [(i, m[1]) for i, line in enumerate(lines) if (m := _ID_LINE_RE.match(line))]:
            start = max((j for j in range(at + 1) if lines[j].lstrip().startswith("{")), default=at)
            end = next((j for j in range(at, len(lines)) if lines[j].rstrip().rstrip(",").endswith("}")), len(lines) - 1)
            commits = [blamed[i] for i in range(start, end + 1) if '"' in lines[i]]
            out.append((id_, None if self.UNCOMMITTED in commits else min(commits, key=lambda c: self.order.get(c, len(self.order)))))
        return out

    @functools.cache
    def changed(self, commit: str) -> frozenset:
        """Every tracked file that differs between `commit` and the working tree."""
        return frozenset((self.git("diff", "--name-only", commit) or "").splitlines())

    @functools.cache
    def text(self, commit: str, path: str) -> tuple | None:
        out = self.git("show", f"{commit}:{path}")
        return None if out is None else tuple(lines_of(out))


def baselines(con, history: History) -> dict:
    """The commit each claim was last verified in: the newer of the commit its own lines last changed in and a review that found it correct. A claim with a line no commit holds is being written, and has none."""
    base = {}
    for (path,) in con.execute("SELECT DISTINCT path FROM nodes WHERE kind = 'claim'"):
        for id_, commit in history.entries(path):
            base[f"claim:{path}#{id_}"] = commit
    for (src,) in con.execute("SELECT DISTINCT src FROM edges WHERE rel = 'reviews'"):
        review = src.removeprefix("file:")
        try:
            verdicts = json.loads(text_of(review) or "{}")["claims"]
        except (ValueError, KeyError, TypeError):
            continue
        located = history.entries(review)
        if [v.get("id") for v in verdicts] != [id_ for id_, _ in located]:
            continue   # entries that do not line up with their ids name no lines, so the review counts for nothing rather than for the wrong claim
        for v, (_, commit) in zip(verdicts, located):
            claim = gen_atlas.reviewed_claim(review, v)
            if v.get("verdict") == "correct" and commit and base.get(claim):
                base[claim] = min(base[claim], commit, key=lambda c: history.order.get(c, len(history.order)))
    return base


def section(lines, anchor: str):
    """A page's section: its anchor's line up to the next heading of its level or above."""
    marks = anchor_lines("\n".join(lines))
    if anchor not in marks:
        return None
    start, level = marks[anchor]
    return lines[start:min((n for n, lv in marks.values() if n > start and 0 < lv <= (level or 6)), default=len(lines))]


def card(text: str, page: str, anchor: str):
    """What the docs build writes a catalog page from at an anchor: the help.json entry of the module whose card it is, else, for the page whole or any other anchor on it, the page's catalog entry and every card on it."""
    try:
        help_ = json.loads(text)
    except ValueError:
        return None
    catalog = page.removeprefix("docs/moonmodules/")
    cards = {m["doc"].partition("#")[2]: m for m in help_.get("modules", {}).values() if m.get("doc", "").partition("#")[0] == catalog}
    return cards[anchor] if anchor and anchor in cards else (help_.get("catalogs", {}).get(catalog), sorted(cards.items(), key=lambda kv: kv[0]))


def state(cited: str, lines: str | None, base: str, history: History, generated=frozenset()) -> tuple[str, str]:
    """A citation against the commit its claim was verified in: `current` when its text is unchanged, `moved` when the same text sits once elsewhere, `changed` otherwise.

    A page the docs build generates holds on disk at most its written intro, so a citation of it compares what help.json writes it from first, then the text on disk."""
    page, _, anchor = cited.partition("#")
    if page in generated and HELP in history.changed(base):
        if card("\n".join(history.text(base, HELP) or ()), page, anchor) != card(text_of(HELP) or "", page, anchor):
            return "changed", f"{cited} changed, as its entry in {HELP}"
    now = text_of(page)
    if page not in history.changed(base) or now is None:
        return "current", ""
    was, now = history.text(base, page), tuple(lines_of(now))
    if was is None:
        return "changed", f"{page} is not in the claim's commit"
    if anchor:
        return ("current", "") if section(was, anchor) == section(now, anchor) else ("changed", f"{page}#{anchor} changed")
    m = _LINES_RE.fullmatch(lines or "")
    if not m:
        return ("current", "") if was == now else ("changed", f"{page} changed")
    first, last = int(m[1]), int(m[2] or m[1])
    block = was[first - 1:last]
    if len(block) < last - first + 1:
        return "changed", f"{page}:{lines} was past the end in the claim's commit"
    if now[first - 1:last] == block:
        return "current", ""
    hits = [i + 1 for i in range(len(now) - len(block) + 1) if now[i:i + len(block)] == block]
    if len(hits) == 1:
        return "moved", f"{page}:{lines} is now {hits[0]}" + (f"-{hits[0] + last - first}" if last > first else "")
    return "changed", f"{page}:{lines} changed"


def stale(con, history: History | None = None) -> list[dict]:
    """Each claim with a citation whose text differs from the text the claim was verified against: `claim stale` when a person re-reads it, `citation moved` when only its lines move."""
    history = history or History()
    base = baselines(con, history)
    generated = frozenset(p for (p,) in con.execute("SELECT path FROM nodes WHERE kind = 'file' AND note = ?", (gen_atlas.GENERATED,)))
    found = [{"rule": "source skipped", "message": why, "file": IGNORE_REVS, "level": "error"} for why in sorted(history.refused)]
    for claim, path in con.execute("SELECT id, path FROM nodes WHERE kind = 'claim' ORDER BY id"):
        if not base.get(claim):
            continue
        states = [state(dst.split(":", 1)[1], lines, base[claim], history, generated)
                  for dst, lines in con.execute("SELECT dst, detail FROM edges WHERE src = ? AND rel = 'cites' ORDER BY rowid", (claim,))]
        notes = [why for st, why in states if st != "current"]
        if notes:
            rule = "claim stale" if any(st == "changed" for st, _ in states) else "citation moved"
            found.append({"rule": rule, "message": f"{claim.removeprefix('claim:')}: {'; '.join(notes)}", **at_claim(path, claim)})
    return found


def dangling(con) -> list[dict]:
    """Each edge with an end no source defines, but a citation, which has its own rule, and a base class, which may live outside the tree."""
    found = []
    for src, rel, dst in con.execute(
            "SELECT DISTINCT src, rel, dst FROM edges WHERE rel NOT IN ('cites', 'derives') "
            "AND (src NOT IN (SELECT id FROM nodes) OR dst NOT IN (SELECT id FROM nodes)) ORDER BY src, rel, dst"):
        at = node(con, src)
        where = {"file": at[2]} if at and at[0] == "file" else {}
        found.append({"rule": "edge to nothing", "message": f"{'' if where else src + ' '}{rel} {dst}, which no source defines", **where})
    return found


def undocumented(con) -> list[dict]:
    """Each module with no doc section."""
    return [{"rule": "module undocumented", "message": f"{mod.removeprefix('module:')} has no doc section in help.json", "level": "error"}
            for (mod,) in con.execute("SELECT id FROM nodes n WHERE kind = 'module' AND NOT EXISTS "
                                      "(SELECT 1 FROM edges e WHERE e.src = n.id AND e.rel = 'documented_by') ORDER BY id")]


def findings(con, history: History | None = None) -> list[dict]:
    return skipped(con) + unresolved(con) + stale(con, history) + dangling(con) + undocumented(con)


def main() -> int:
    print(gen_atlas.refresh(), file=sys.stderr)
    with contextlib.closing(sqlite3.connect(f"{gen_atlas.DB.as_uri()}?mode=ro", uri=True)) as con:
        found = findings(con)
    _findings.write("check_atlas", RULES, found)

    errors = [f for f in found if f.get("level") == "error"]
    by_rule = {r: [f for f in found if f["rule"] == r] for r in RULES}
    print("Atlas check: " + ", ".join(f"{len(fs)} {r}" for r, fs in by_rule.items()))
    for rule, fs in by_rule.items():
        if fs:
            print(f"\n{rule}: {RULES[rule]['fix']}")
            for f in fs:
                print(f"  {f.get('file', '')}{':' + str(f['line']) if 'line' in f else ''}  {f['message']}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
