#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6", "jsonschema>=4"]
# ///
# gen_atlas's dependencies: a query first brings the graph up to date, and gen_atlas builds it.
"""Ask MoonAtlas what a module or a file is, why it is shaped that way, and what a change to it touches.

Every answer is rows of the graph gen_atlas builds, each pointing at the one home of what it says: a header, a doc section, a test, a MoonWalk step. A query first brings the graph up to date with the tree, one stat pass when nothing changed, so an answer always describes the tree as it is.

A name is what a maintainer types: a module or a class (`Layer`), a file by its path (`src/light/layers/Layer.h`), its name or its stem (`Layer.h`), a process (`commit`), a step (`commit/specs`), a section (`docs/moonwalk.md#commit`), or a node id as `what` prints it.

Usage:
    uv run moondeck/docs/show_atlas.py what Layer            # the node and every edge in and out of it
    uv run moondeck/docs/show_atlas.py why Layer             # its doc sections and the code sections of its header and implementation
    uv run moondeck/docs/show_atlas.py impact MoonModule.h   # what includes or imports it, the tests and steps covering it, the docs and claims about it
    uv run moondeck/docs/show_atlas.py tests Layer           # the tests that pin it and the MoonWalk steps they prove
    uv run moondeck/docs/show_atlas.py rules                 # each standards section and the checks that enforce it
    uv run moondeck/docs/show_atlas.py gaps                  # claims no page holds yet, by the page each belongs on
"""

import argparse
import contextlib
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import gen_atlas  # noqa: E402

SCENARIOS = f"file:{gen_atlas._test_metadata.SCENARIO_DIR.relative_to(gen_atlas._test_metadata.ROOT).as_posix()}/"


def node(con, id_: str):
    return con.execute("SELECT kind, name, path, line, note FROM nodes WHERE id = ?", (id_,)).fetchone()


def label(con, id_: str) -> str:
    """One line naming a node by where it lives: a path to open, a line to jump to, a step and what it says."""
    row = node(con, id_)
    if row is None:
        return f"{id_} (no source defines it)"
    kind, name, path, line, _ = row
    if kind == "file":
        return path
    if kind == "section":
        return f"{path}:{line}  {name}" if line else id_.removeprefix("doc:")
    if kind == "class":
        return f"{name}  {path}:{line}"
    if kind == "claim":
        return f"{id_.removeprefix('claim:')}  {verdict(con, id_)}{name}"
    if kind in ("process", "step") and name:
        return f"{id_}  {name}"
    return id_


def review(con, claim: str) -> dict:
    """The blind review's verdict on a claim, the latest when two reviews checked it, or an empty dict for a claim no review sampled."""
    row = con.execute("SELECT detail FROM edges WHERE dst = ? AND rel = 'reviews' ORDER BY rowid DESC", (claim,)).fetchone()
    return json.loads(row[0]) if row and row[0] else {}


def verdict(con, claim: str) -> str:
    """The prefix a reviewed claim carries when the review found something to say: what it found, then the reviewer's reason. A claim reviewed correct, or never sampled, carries none."""
    r = review(con, claim)
    found = [f for f, bad in ((r.get("verdict"), r.get("verdict") not in (None, "correct")),
                              ("home wrong", r.get("home_ok") is False),
                              ("citation beside it", r.get("cites_support") is False)) if bad]
    return f"[{', '.join(found)}: {r.get('reason', '')}] " if found else ""


def targets(con, id_: str, rel: str) -> list[str]:
    return [r[0] for r in con.execute("SELECT DISTINCT dst FROM edges WHERE src = ? AND rel = ?", (id_, rel))]


def sources(con, id_: str, rel: str) -> list[str]:
    return [r[0] for r in con.execute("SELECT DISTINCT src FROM edges WHERE dst = ? AND rel = ?", (id_, rel))]


def resolve(con, name: str) -> list[str]:
    """The nodes a typed name means: every kind it names exactly, else the files with that name or stem."""
    exact = [i for i in (name, f"module:{name}", f"class:{name}", f"process:{name}", f"step:{name}", f"file:{name}", f"doc:{name}")
             if node(con, i)]
    if exact:
        return list(dict.fromkeys(exact))
    found = [i for i, n in con.execute("SELECT id, name FROM nodes WHERE kind = 'file' ORDER BY path") if name in (n, Path(n).stem)]
    if not found:
        raise LookupError(f"MoonAtlas: nothing is named {name}; a name is a module, a class, a file, a process, a step or a section")
    return found


def aliases(con, ids: list[str]) -> set[str]:
    """A file stands for what it defines, a module and its class are one thing, both live in a header, and a header's implementation is the source file of its stem that includes it: a question about one is asked of all."""
    named = set(ids)
    for i in ids:
        named |= set(targets(con, i, "defines")) | set(sources(con, i, "defined_in"))
    for i in list(named):
        named |= set(targets(con, i, "is")) | set(sources(con, i, "is"))
    for i in list(named):
        named |= set(targets(con, i, "defined_in")) | set(sources(con, i, "defines"))
    for i in [i for i in named if i.startswith("file:")]:
        named |= {s for s in sources(con, i, "includes") if Path(s).stem == Path(i).stem}
    return named


def included_by(con, files: set[str]) -> set[str]:
    """Every file that reaches one of these through `#include` or a Python import, however many steps away: the recursive query of an adjacency list."""
    reach: set[str] = set()
    for f in files:
        reach |= {r[0] for r in con.execute(
            "WITH RECURSIVE up(id) AS (SELECT ? UNION SELECT e.src FROM edges e JOIN up ON e.dst = up.id AND e.rel IN ('includes', 'imports')) "
            "SELECT id FROM up", (f,))}
    return reach - files


def order(con, id_: str) -> tuple:
    """Files by path, and a file's sections by line, as a reader walks them."""
    row = node(con, id_)
    return (row[2] or id_, row[3] or 0, id_) if row else (id_, 0, id_)


def block(con, title: str, ids) -> list[str]:
    if not ids:
        return [f"{title}: none"]
    return [f"{title} ({len(ids)})", *(f"  {label(con, i)}" for i in sorted(ids, key=lambda i: order(con, i)))]


def what(con, name: str) -> list[str]:
    """Each node the name means, then every edge leaving it and every edge arriving at it."""
    lines = []
    for i in resolve(con, name):
        kind, name_, path, line, note = node(con, i)
        lines.append(i + (f"  {path}:{line}" if line else "") + (f"  {name_}" if kind in ("claim", "step") else "")
                     + (f": {note}" if note and kind != "claim" else ""))
        for rel, dst, detail in con.execute("SELECT DISTINCT rel, dst, detail FROM edges WHERE src = ? ORDER BY rel, dst", (i,)):
            lines.append(f"  {rel} -> {label(con, dst)}" + (f" [{detail}]" if detail else ""))
        for rel, src, detail in con.execute("SELECT DISTINCT rel, src, detail FROM edges WHERE dst = ? ORDER BY rel, src", (i,)):
            lines.append(f"  {rel} <- {label(con, src)}" + (f" [{detail}]" if detail else ""))
    return lines


def why(con, name: str) -> list[str]:
    """Where the reasons are written: the doc sections it is documented by, and every `/// ##` section of its header and implementation."""
    named = aliases(con, resolve(con, name))
    code = {i for i in named if i.startswith("file:") and i.endswith(gen_atlas.CODE)}
    return (block(con, "documented by", {d for i in named for d in targets(con, i, "documented_by")})
            + block(con, "explained in code", {s for f in code for s in sources(con, f, "in")}))


def impact(con, name: str) -> list[str]:
    """What a change touches. Code compiles through every includer and a script runs through every importer, so tests and steps follow both all the way; prose describes what it names, so docs and claims stay with the name."""
    named = aliases(con, resolve(con, name))
    files = {i for i in named if i.startswith("file:")}
    reach = included_by(con, files)
    test_files = {r[0] for r in con.execute("SELECT DISTINCT src FROM edges WHERE rel = 'tests'")}
    built = named | reach | {d for f in reach for d in targets(con, f, "defines")} | {s for f in reach for s in sources(con, f, "defined_in")}
    tested = {s for i in built for s in sources(con, i, "tests")} | (reach & test_files)
    steps = {d for t in tested for d in targets(con, t, "proves")} | {s for i in named | reach for s in sources(con, i, "runs")}
    documented = {d for i in named for d in targets(con, i, "documented_by")}
    docs = documented | {s for i in named for s in sources(con, i, "mentions")}
    # A page that only mentions the name holds claims about other things, so a claim counts when it cites the code or the section that documents it.
    cited = files | {s for f in files for s in sources(con, f, "in")} | documented
    claims = {s for i in cited for s in sources(con, i, "cites")}
    return (block(con, "included or imported by", reach - test_files)
            + block(con, "unit tests", {t for t in tested if not t.startswith(SCENARIOS)})
            + block(con, "scenarios", {t for t in tested if t.startswith(SCENARIOS)})
            + block(con, "MoonWalk steps", steps)
            + block(con, "docs", docs)
            + block(con, "claims", claims))


def tests(con, name: str) -> list[str]:
    """The tests that name it, each with the MoonWalk steps it proves."""
    named = aliases(con, resolve(con, name))
    found = sorted({s for i in named for s in sources(con, i, "tests")}, key=lambda t: label(con, t))
    if not found:
        return ["tests: none"]
    lines = []
    for t in found:
        lines.append(label(con, t))
        lines += [f"  proves {label(con, s)}" for s in sorted(targets(con, t, "proves"))]
    return lines


def rules(con) -> list[str]:
    """Each section of every standards page a check links, with the checks that enforce it; a section no check links is left to review."""
    by_target: dict[str, list[str]] = {}
    for src, dst in con.execute("SELECT src, dst FROM edges WHERE rel = 'enforces' ORDER BY src"):
        by_target.setdefault(dst, []).append(Path(src).stem)
    pages = sorted({t.split(":", 1)[1].partition("#")[0] for t in by_target})
    lines = []
    for page in pages:
        lines.append(page + (f"  <- {', '.join(by_target[f'file:{page}'])} (the whole page)" if f"file:{page}" in by_target else ""))
        for (section,) in con.execute("SELECT id FROM nodes WHERE kind = 'section' AND path = ? ORDER BY id", (page,)):
            checks = by_target.get(section)
            lines.append(f"  #{section.partition('#')[2]}  " + (f"<- {', '.join(checks)}" if checks else "review"))
    return lines


def gaps(con) -> list[str]:
    """The claims whose home is none: sentences the docs are missing, grouped by the page where each belongs, with what it cites. A claim its review found already written is no gap."""
    by_home: dict[str, list[str]] = {}
    for id_, note in con.execute("SELECT id, note FROM nodes WHERE kind = 'claim' ORDER BY id"):
        meta = json.loads(note or "{}")
        if meta.get("home") == "none" and review(con, id_).get("home_ok") is not False:
            by_home.setdefault(meta.get("gap_home") or "(no page named)", []).append(id_)
    if not by_home:
        return ["gaps: none"]
    lines = []
    for home in sorted(by_home):
        lines.append(home)
        for id_ in by_home[home]:
            lines.append(f"  {label(con, id_)}")
            lines += [f"    cites {label(con, d)}" + (f" [{detail}]" if detail else "")
                      for d, detail in con.execute("SELECT dst, detail FROM edges WHERE src = ? AND rel = 'cites'", (id_,))]
    return lines


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="command", required=True)
    for fn in (what, why, impact, tests):
        p = sub.add_parser(fn.__name__, help=fn.__doc__.splitlines()[0])
        p.add_argument("name", help="a module, a class, a file, a process, a step or a section")
        p.set_defaults(fn=fn)
    for fn in (rules, gaps):
        sub.add_parser(fn.__name__, help=fn.__doc__.splitlines()[0]).set_defaults(fn=fn)
    args = ap.parse_args()

    print(gen_atlas.refresh(), file=sys.stderr)
    with contextlib.closing(sqlite3.connect(f"{gen_atlas.DB.as_uri()}?mode=ro", uri=True)) as con:
        try:
            lines = args.fn(con, args.name) if "name" in args else args.fn(con)
        except LookupError as e:
            print(e, file=sys.stderr)
            return 1
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
