#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6", "jsonschema>=4", "markdown>=3"]
# ///
# gen_atlas's dependencies: a query first brings the graph up to date, and gen_atlas builds it.
"""Ask MoonAtlas what a module or a file is, why it is shaped that way, and what a change to it touches.

Every answer is rows of the graph gen_atlas builds, each pointing at the one home of what it says: a header, a doc section, a test, a MoonWalk step. A query first brings the graph up to date with the tree, one stat pass when nothing changed, so an answer describes the tree as it is, or says on stderr that it comes from the last graph.

An answer is data, `{verb, query, graph, groups: [{title, node?, rows}]}` under `show_atlas.schema.json`, and the text a person reads is one view of it, `--json` the other: a row carries what its line names (the node's id, kind, name, path and line, a claim's review) and, where a node's own edges matter, the rows they lead to. `cloud`, the 3D view's feed, is data alone, `{verb, query, graph, nodes, links}`, outside the schema.

A name is what a maintainer types: a module or a class (`Layer`), a file by its path (`src/light/layers/Layer.h`), its name or its stem (`Layer.h`), a process (`commit`), a step (`commit/specs`), a section (`docs/moonwalk.md#commit`), or a node id as `what` prints it.

Usage:
    uv run moondeck/docs/show_atlas.py what Layer            # the node and every edge in and out of it
    uv run moondeck/docs/show_atlas.py why Layer             # its doc sections and the code sections of its header and implementation
    uv run moondeck/docs/show_atlas.py impact MoonModule.h   # what includes or imports it, the tests and steps covering it, the docs and claims about it
    uv run moondeck/docs/show_atlas.py tests Layer           # the tests that pin it and the MoonWalk steps they prove
    uv run moondeck/docs/show_atlas.py rules                 # each standards section and the checks that enforce it
    uv run moondeck/docs/show_atlas.py gaps                  # claims no page holds yet, by the page each belongs on
    uv run moondeck/docs/show_atlas.py impact Layer --json   # the same answer as data
    uv run moondeck/docs/show_atlas.py cloud --around Layer --depth 2   # the nodes within two edges of it and their links, for the 3D view
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
SCHEMA = Path(__file__).resolve().parent / "show_atlas.schema.json"


def node(con, id_: str):
    return con.execute("SELECT kind, name, path, line, note FROM nodes WHERE id = ?", (id_,)).fetchone()


def review(con, claim: str) -> dict:
    """The blind review's verdict on a claim, the latest when two reviews checked it, or an empty dict for a claim no review sampled."""
    row = con.execute("SELECT detail FROM edges WHERE dst = ? AND rel = 'reviews' ORDER BY rowid DESC", (claim,)).fetchone()
    return json.loads(row[0]) if row and row[0] else {}


def row(con, id_: str, **edge) -> dict:
    """A node as an answer carries it: what its line names, a claim's review, and the edge that reached it when one did. A node no source defines has no kind."""
    found = node(con, id_)
    via = {k: v for k, v in edge.items() if v is not None}
    if found is None:
        return {"id": id_, "kind": None, **via}
    kind, name, path, line, note = found
    out = {"id": id_, "kind": kind, "name": name, "path": path, "line": line}
    if kind == "claim":
        out["review"] = review(con, id_)
    elif note:
        out["note"] = note
    return {**out, **via}


def prefix(found: dict) -> str:
    """The prefix a reviewed claim carries when the review found something to say: what it found, then the reviewer's reason. A claim reviewed correct, or never sampled, carries none."""
    said = [f for f, bad in ((found.get("verdict"), found.get("verdict") not in (None, "correct")),
                             ("home wrong", found.get("home_ok") is False),
                             ("citation beside it", found.get("cites_support") is False)) if bad]
    return f"[{', '.join(said)}: {found.get('reason', '')}] " if said else ""


def verdict(con, claim: str) -> str:
    return prefix(review(con, claim))


def label(r: dict) -> str:
    """One line naming a row by where it lives: a path to open, a line to jump to, a step and what it says."""
    kind, id_ = r["kind"], r["id"]
    if kind is None:
        return f"{id_} (no source defines it)"
    if kind == "file":
        return r["path"]
    if kind == "section":
        return f"{r['path']}:{r['line']}  {r['name']}" if r["line"] else id_.removeprefix("doc:")
    if kind == "class":
        return f"{r['name']}  {r['path']}:{r['line']}"
    if kind == "claim":
        return f"{id_.removeprefix('claim:')}  {prefix(r['review'])}{r['name']}"
    if kind in ("process", "step") and r["name"]:
        return f"{id_}  {r['name']}"
    return id_


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
    found = node(con, id_)
    return (found[2] or id_, found[3] or 0, id_) if found else (id_, 0, id_)


def group(con, title: str, ids) -> dict:
    return {"title": title, "rows": [row(con, i) for i in sorted(ids, key=lambda i: order(con, i))]}


def answer(verb: str, query: str | None, groups: list[dict]) -> dict:
    return {"verb": verb, "query": query, "groups": groups}


def what(con, name: str) -> dict:
    """Each node the name means, then every edge leaving it and every edge arriving at it."""
    groups = []
    for i in resolve(con, name):
        rows = [row(con, dst, rel=rel, dir="out", detail=detail) for rel, dst, detail in
                con.execute("SELECT DISTINCT rel, dst, detail FROM edges WHERE src = ? ORDER BY rel, dst", (i,))]
        rows += [row(con, src, rel=rel, dir="in", detail=detail) for rel, src, detail in
                 con.execute("SELECT DISTINCT rel, src, detail FROM edges WHERE dst = ? ORDER BY rel, src", (i,))]
        groups.append({"title": i, "node": row(con, i), "rows": rows})
    return answer("what", name, groups)


def why(con, name: str) -> dict:
    """Where the reasons are written: the doc sections it is documented by, and every `/// ##` section of its header and implementation."""
    named = aliases(con, resolve(con, name))
    code = {i for i in named if i.startswith("file:") and i.endswith(gen_atlas.CODE)}
    return answer("why", name, [group(con, "documented by", {d for i in named for d in targets(con, i, "documented_by")}),
                                group(con, "explained in code", {s for f in code for s in sources(con, f, "in")})])


def impact(con, name: str) -> dict:
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
    return answer("impact", name, [group(con, "included or imported by", reach - test_files),
                                   group(con, "unit tests", {t for t in tested if not t.startswith(SCENARIOS)}),
                                   group(con, "scenarios", {t for t in tested if t.startswith(SCENARIOS)}),
                                   group(con, "MoonWalk steps", steps),
                                   group(con, "docs", docs),
                                   group(con, "claims", claims)])


def tests(con, name: str) -> dict:
    """The tests that name it, each with the MoonWalk steps it proves."""
    named = aliases(con, resolve(con, name))
    found = sorted({s for i in named for s in sources(con, i, "tests")}, key=lambda t: label(row(con, t)))
    rows = [{**row(con, t), "rows": [row(con, s, rel="proves", dir="out") for s in sorted(targets(con, t, "proves"))]} for t in found]
    return answer("tests", name, [{"title": "tests", "rows": rows}])


def rules(con) -> dict:
    """Each section of every standards page a check links, with the checks that enforce it; a section no check links is left to review."""
    by_target: dict[str, list[str]] = {}
    for src, dst in con.execute("SELECT src, dst FROM edges WHERE rel = 'enforces' ORDER BY src"):
        by_target.setdefault(dst, []).append(src)

    def enforced(target: str) -> list[dict]:
        return [row(con, src, rel="enforces", dir="in") for src in by_target.get(target, [])]

    groups = []
    for page in sorted({t.split(":", 1)[1].partition("#")[0] for t in by_target}):
        sections = [{**row(con, s), "rows": enforced(s)} for (s,) in
                    con.execute("SELECT id FROM nodes WHERE kind = 'section' AND path = ? ORDER BY id", (page,))]
        groups.append({"title": page, "node": {**row(con, f"file:{page}"), "rows": enforced(f"file:{page}")}, "rows": sections})
    return answer("rules", None, groups)


def gaps(con) -> dict:
    """The claims whose home is none: sentences the docs are missing, grouped by the page where each belongs, with what it cites. A claim its review found already written is no gap."""
    by_home: dict[str, list[str]] = {}
    for id_, note in con.execute("SELECT id, note FROM nodes WHERE kind = 'claim' ORDER BY id"):
        meta = json.loads(note or "{}")
        if meta.get("home") == "none" and review(con, id_).get("home_ok") is not False:
            by_home.setdefault(meta.get("gap_home") or "(no page named)", []).append(id_)
    groups = [{"title": home, "rows": [{**row(con, id_), "rows": [row(con, d, rel="cites", dir="out", detail=detail) for d, detail in
                                                                  con.execute("SELECT dst, detail FROM edges WHERE src = ? AND rel = 'cites'", (id_,))]}
                                       for id_ in by_home[home]]}
              for home in sorted(by_home)]
    return answer("gaps", None, groups)


def edge(r: dict) -> str:
    return f"  {r['rel']} {'->' if r['dir'] == 'out' else '<-'} {label(r)}" + (f" [{r['detail']}]" if r.get("detail") else "")


def stems(rows: list[dict]) -> str:
    return ", ".join(Path(r["id"]).stem for r in rows)


def text(a: dict) -> list[str]:
    """The lines a person reads, rendered from the answer alone."""
    verb, groups = a["verb"], a["groups"]
    if verb == "what":
        lines = []
        for g in groups:
            n = g["node"]
            lines.append(n["id"] + (f"  {n['path']}:{n['line']}" if n["line"] else "") + (f"  {n['name']}" if n["kind"] in ("claim", "step") else "")
                         + (f": {n['note']}" if n.get("note") else ""))
            lines += [edge(r) for r in g["rows"]]
        return lines
    if verb == "tests":
        rows = groups[0]["rows"]
        return [line for r in rows for line in (label(r), *(f"  proves {label(s)}" for s in r["rows"]))] or ["tests: none"]
    if verb == "rules":
        lines = []
        for g in groups:
            whole = g["node"]["rows"]
            lines.append(g["title"] + (f"  <- {stems(whole)} (the whole page)" if whole else ""))
            lines += [f"  #{s['id'].partition('#')[2]}  " + (f"<- {stems(s['rows'])}" if s["rows"] else "review") for s in g["rows"]]
        return lines
    if verb == "gaps":
        lines = []
        for g in groups:
            lines.append(g["title"])
            for c in g["rows"]:
                lines.append(f"  {label(c)}")
                lines += [f"    cites {label(d)}" + (f" [{d['detail']}]" if d.get("detail") else "") for d in c["rows"]]
        return lines or ["gaps: none"]
    return [line for g in groups for line in
            ([f"{g['title']}: none"] if not g["rows"] else [f"{g['title']} ({len(g['rows'])})", *(f"  {label(r)}" for r in g["rows"])])]


NAMED, BARE = (what, why, impact, tests), (rules, gaps)   # the verbs that take a name, and those that do not


def refreshed() -> dict:
    """Bring the graph up to date with the tree, and say how it went: fresh with the refresh's line, or why it was not and whether the last graph still answers."""
    try:
        return {"fresh": True, "note": gen_atlas.refresh()}
    except Exception as e:  # a failed refresh leaves the last graph, an answer about the tree as it last read it
        return {"fresh": False, "note": f"MoonAtlas: not refreshed: {e!r}" + ("; answering from the last graph" if gen_atlas.DB.exists() else "")}


def area(con, r: dict) -> str:
    """The folder a node orbits in the 3D view: the first two parts of its path; a claim orbits what it cites first, a node with no path the file it is defined in, a control its module's, else its kind."""
    path = r.get("path")
    if r["kind"] == "claim" or not path:
        for rel in ("cites", "defined_in"):
            hit = con.execute("SELECT n.path FROM edges e JOIN nodes n ON n.id = e.dst WHERE e.src = ? AND e.rel = ? AND n.path IS NOT NULL "
                              "ORDER BY e.rowid LIMIT 1", (r["id"], rel)).fetchone()
            if hit:
                path = hit[0]
                break
    if not path and r["kind"] == "control":
        owner = con.execute("SELECT src FROM edges WHERE dst = ? AND rel = 'has_control' ORDER BY rowid LIMIT 1", (r["id"],)).fetchone()
        if owner:
            return area(con, row(con, owner[0]))
    return "/".join(path.split("/")[:2]) if path else r["kind"]


def cloud(con, around: str | None = None, depth: int = 1) -> dict:
    """The graph as points and links for the 3D view: every node, or the nodes within `depth` edges of what a name means; a link is one pair of nodes, weighted by how many edges join them, and an edge to a node no source defines draws nothing."""
    known = {r[0] for r in con.execute("SELECT id FROM nodes")}
    if around is None:
        ids = known
    else:
        ids = set(resolve(con, around))
        frontier = set(ids)
        for _ in range(depth):
            reached = {r[0] for i in frontier for r in con.execute("SELECT dst FROM edges WHERE src = ? UNION SELECT src FROM edges WHERE dst = ?", (i, i))}
            frontier = (reached & known) - ids
            ids |= frontier
    links: dict[tuple, dict] = {}
    for src, rel, dst in con.execute("SELECT src, rel, dst FROM edges ORDER BY rowid"):
        if src in ids and dst in ids and src != dst:
            pair = (src, dst) if src < dst else (dst, src)
            link = links.setdefault(pair, {"source": pair[0], "target": pair[1], "rels": [], "weight": 0})
            link["weight"] += 1
            if rel not in link["rels"]:
                link["rels"].append(rel)
    nodes = []
    for i in sorted(ids):
        r = row(con, i)
        nodes.append({k: r[k] for k in ("id", "kind", "name", "path", "line")} | {"area": area(con, r)})
    return {"verb": "cloud", "query": around, "nodes": nodes, "links": list(links.values())}


def mark_stale(con, nodes: list[dict]) -> None:
    """Mark each claim check_atlas reads stale, so the view colors it: a sentence whose cited text changed since it was verified."""
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "check"))
    import check_atlas
    stale = {f["node"] for f in check_atlas.stale(con) if f["rule"] == "claim stale"}
    for n in nodes:
        if n["id"] in stale:
            n["stale"] = True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    out = argparse.ArgumentParser(add_help=False)
    out.add_argument("--json", action="store_true", help="print the answer as data, under show_atlas.schema.json, instead of text")
    sub = ap.add_subparsers(dest="command", required=True)
    for fn in (*NAMED, *BARE):
        p = sub.add_parser(fn.__name__, help=fn.__doc__.splitlines()[0], parents=[out])
        if fn in NAMED:
            p.add_argument("name", help="a module, a class, a file, a process, a step or a section")
        p.set_defaults(fn=fn)
    c = sub.add_parser("cloud", help=cloud.__doc__.splitlines()[0])
    c.add_argument("--around", help="a name, as for `what`; every node when left out")
    c.add_argument("--depth", type=int, default=1, choices=range(4), help="how many edges out from --around (default: 1)")
    args = ap.parse_args()

    graph = refreshed()
    print(graph["note"], file=sys.stderr)
    if not gen_atlas.DB.exists():
        print("MoonAtlas: no graph to answer from; run gen_atlas to see why it cannot build one", file=sys.stderr)
        return 1
    with contextlib.closing(sqlite3.connect(f"{gen_atlas.DB.as_uri()}?mode=ro", uri=True)) as con:
        try:
            if args.command == "cloud":   # the 3D view's feed is data alone
                found = cloud(con, args.around, args.depth)
                mark_stale(con, found["nodes"])
                print(json.dumps({**found, "graph": graph}))
                return 0
            found = args.fn(con, args.name) if "name" in args else args.fn(con)
        except LookupError as e:
            print(e, file=sys.stderr)
            return 1
        except sqlite3.DatabaseError as e:
            print(f"MoonAtlas: the last graph cannot be read ({e}); run gen_atlas to rebuild it", file=sys.stderr)
            return 1
    print(json.dumps({**found, "graph": graph}, indent=1) if args.json else "\n".join(text(found)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
