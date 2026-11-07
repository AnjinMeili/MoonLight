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

Each verb is declared once, in TOOLS: the command line, its --help, `--schema` and the MCP tools serve_atlas lists are read from the declarations. With no verb it lists the verbs, each with an example; options narrow an answer (`--only` its groups, `--kind`, `--rel` and `--stale` its rows, `--limit` how many), and the lines after it say what was left out and which questions follow.
"""

import contextlib
import difflib
import inspect
import json
import shlex
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import _verbs  # noqa: E402
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
        names = {n for (n,) in con.execute("SELECT name FROM nodes WHERE kind IN ('module', 'class', 'file', 'process') AND name IS NOT NULL")}
        close = difflib.get_close_matches(name, sorted(names), n=3, cutoff=0.75)
        raise LookupError(f"MoonAtlas: nothing is named {name}" + (f"; the names closest to it: {', '.join(close)}" if close else "")
                          + ". A name is a module, a class, a file, a process, a step or a section; the schema verb (atlas_schema) lists the kinds."
                          + " When none of these is what was meant, ask rather than try other spellings.")
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
    if verb == "schema":
        return [line for g in groups for line in (f"{g['title']} ({len(g['rows'])})", *(f"  {r['id']:<14} {r['count']:>6}  {r['name']}" for r in g["rows"]))]
    if verb == "gaps":
        lines = []
        for g in groups:
            lines.append(g["title"])
            for c in g["rows"]:
                lines.append(f"  {label(c)}")
                lines += [f"    cites {label(d)}" + (f" [{d['detail']}]" if d.get("detail") else "") for d in c["rows"]]
        return lines or ["gaps: none"]
    return [line for g in groups for line in
            ([f"{g['title']}: none" if "total" not in g else f"{g['title']} (0 of {g['total']})"] if not g["rows"] else [f"{g['title']} ({len(g['rows'])}" + (f" of {g['total']})" if "total" in g else ")"),
                                                            *(f"  {label(r)}" for r in g["rows"])])]


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


def stale_claims(con) -> set[str]:
    """The claims check_atlas reads stale: sentences whose cited text changed since they were verified, read through git."""
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "check"))
    import check_atlas
    return {f["node"] for f in check_atlas.stale(con) if f["rule"] == "claim stale"}


def mark_stale(con, nodes: list[dict]) -> None:
    """Mark each claim check_atlas reads stale, so the view colors it."""
    stale = stale_claims(con)
    for n in nodes:
        if n["id"] in stale:
            n["stale"] = True


def schema(con) -> dict:
    """Each kind of node and each relation the graph holds, with its count and what it means: the names --kind and --rel take."""
    def counted(sql, meanings, kind):
        return [{"id": name, "kind": kind, "name": meanings.get(name, "(no meaning declared in gen_atlas)"), "count": n} for name, n in con.execute(sql)]
    return answer("schema", None, [{"title": "kinds", "rows": counted("SELECT kind, count(*) FROM nodes GROUP BY kind ORDER BY kind", gen_atlas.KINDS, "kind")},
                                   {"title": "relations", "rows": counted("SELECT rel, count(*) FROM edges GROUP BY rel ORDER BY rel", gen_atlas.RELS, "relation")}])


def version(con) -> str:
    """The graph's version: the commit it was built at and its build fingerprint, so an answer read from another graph reads as another."""
    meta = dict(con.execute("SELECT key, value FROM meta"))
    return f"{meta.get('commit') or '?'}:{(meta.get('fingerprint') or 'partial')[:12]}"


@contextlib.contextmanager
def opened():
    """The graph, read-only; a graph missing or unreadable is a question nothing answers, said with what to run."""
    if not gen_atlas.DB.exists():
        raise LookupError("MoonAtlas: no graph to answer from; run `uv run moondeck/docs/gen_atlas.py` to see why it cannot build one")
    with contextlib.closing(sqlite3.connect(f"{gen_atlas.DB.as_uri()}?mode=ro", uri=True)) as con:
        try:
            yield con
        except sqlite3.DatabaseError as e:
            raise LookupError(f"MoonAtlas: the last graph cannot be read ({e}); run `uv run moondeck/docs/gen_atlas.py --force` to rebuild it") from e


def answered(fn, *args) -> dict:
    """Refresh, then ask: the answer with the graph it was read from, the refresh's line on stderr."""
    graph = refreshed()
    print(graph["note"], file=sys.stderr)
    with opened() as con:
        a = fn(con, *args)
        a["graph"] = {**graph, "version": version(con)}
    return a


LIMIT = 20
SCRIPT = "uv run moondeck/docs/show_atlas.py"
# The groups --only picks among, by a word each, for the verbs whose answer combines several.
GROUPS = {"impact": {"includers": "included or imported by", "unit-tests": "unit tests", "scenarios": "scenarios", "steps": "MoonWalk steps", "docs": "docs", "claims": "claims"},
          "why": {"docs": "documented by", "code": "explained in code"}}


def refine(a: dict, only=None, kind=None, rel=None, stale=None, limit=LIMIT) -> dict:
    """Narrow an answer as the options ask: the groups `only` names, then the rows of the kinds, relations and stale claims asked for, then at most `limit` rows a group, saying how many were left out."""
    if only:
        titles = {GROUPS[a["verb"]][k] for k in only}
        a["groups"] = [g for g in a["groups"] if g["title"] in titles]
    left, most = 0, 0
    for g in a["groups"]:
        rows = [r for r in g["rows"] if (not kind or r["kind"] in kind) and (not rel or r.get("rel") in rel) and (stale is None or r["id"] in stale)]
        g["rows"] = rows[:limit]   # a limit of None keeps every row
        if limit is not None and len(rows) > limit:
            g["total"] = len(rows)
            left, most = left + len(rows) - limit, max(most, len(rows))
    if left:
        a["more"] = {"left": left, "limit": most}
    return a


def footer(a: dict) -> list[str]:
    """What an answer left out and the option that brings it, then the questions that follow it."""
    lines = [f"Left out {a['more']['left']} row(s) past the limit; all of them: --limit {a['more']['limit']}"] if a.get("more") else []
    if a.get("next"):
        lines.append("Next: " + " | ".join(f"{SCRIPT} {n['verb']}" + (f" {shlex.quote(n['name'])}" if n.get("name") else " <name>" if n["named"] else "")
                                           for n in a["next"]))
    return lines


def asked(fn, follow: list[str], limited: bool = True):
    """A verb's run: refresh, ask, refine, and name the verbs that follow with the same name."""
    named = "name" in inspect.signature(fn).parameters

    def run(name=None, only=None, kind=None, rel=None, stale=False, limit=LIMIT if limited else None):
        a = answered(fn, *([name] if named else []))
        if stale:
            with opened() as con:
                a = refine(a, only, kind, rel, stale_claims(con), limit)
        else:
            a = refine(a, only, kind, rel, None, limit)
        takes = {v["name"]: "name" in v["params"] for v in TOOLS}
        a["next"] = [{"verb": v, "named": takes[v], **({"name": name} if takes[v] and name else {})} for v in follow]
        return a
    return run


NAME = {"type": "string", "positional": True, "required": True, "description": "a module, class, file, process, step, section or node id: Layer, Layer.h, commit/specs"}
FILTERS = {"kind": {"type": "array", "items": {"enum": list(gen_atlas.KINDS)}, "description": "only rows of these kinds"},
           "rel": {"type": "array", "items": {"enum": list(gen_atlas.RELS)}, "description": "only rows reached by these relations"},
           "stale": {"type": "boolean", "default": False, "description": "only claims whose cited text changed since they were verified"},
           "limit": {"type": "integer", "minimum": 0, "default": LIMIT, "description": "rows a group at most"}}


def rows(*names: str) -> dict:
    """The row filters a verb takes: those that can change its answer, and the limit."""
    return {n: FILTERS[n] for n in (*names, "limit")}


HINTS = {"readOnlyHint": True, "destructiveHint": False, "idempotentHint": True, "openWorldHint": False}
READS = ["the graph, build/atlas/atlas.sqlite", "git, for --stale"]
WRITES = ["build/atlas/atlas.sqlite, rebuilt when the tree changed since it was built"]
EXITS = {0: "answered", 1: "the name names nothing, or no graph can be read: the message says which and what to ask instead", 2: "a usage error"}


def declared(fn, examples: list[str], follow: list[str], params: dict, render=None) -> dict:
    return {"name": fn.__name__, "run": asked(fn, follow, "limit" in params), "text": render or (lambda a: text(a) + footer(a)), "description": fn.__doc__, "params": params,
            "annotations": HINTS, "reads": READS, "writes": WRITES, "examples": examples, "exits": EXITS, "next": follow}


def only(verb: str) -> dict:
    return {"only": {"type": "array", "items": {"enum": list(GROUPS[verb])}, "description": "only these groups"}}


def run_cloud(around=None, depth=1) -> dict:
    """The cloud with its stale claims marked, and the graph it was read from."""
    graph = refreshed()
    print(graph["note"], file=sys.stderr)
    with opened() as con:
        found = cloud(con, around, depth)
        mark_stale(con, found["nodes"])
        return {**found, "graph": {**graph, "version": version(con)}}


TOOLS = [
    declared(what, ["what Layer", "what Layer --rel tests,documented_by"], ["why", "impact", "tests"], {"name": NAME, **rows("kind", "rel", "stale")}),
    declared(why, ["why Layer", "why Layer --only code"], ["what", "impact"], {"name": NAME, **only("why"), **rows("kind")}),
    declared(impact, ["impact MoonModule.h", "impact Layer.h --only claims --stale"], ["tests", "why"], {"name": NAME, **only("impact"), **rows("kind", "stale")}),
    declared(tests, ["tests Layer"], ["impact"], {"name": NAME, **rows()}),
    declared(rules, ["rules", "rules --limit 5"], ["what"], rows()),
    declared(gaps, ["gaps", "gaps --stale"], ["what"], rows("stale")),
    declared(schema, ["schema"], ["what"], {}),
    {"name": "cloud", "run": run_cloud, "text": None, "description": cloud.__doc__, "serve": False, "annotations": HINTS, "reads": READS, "writes": WRITES,
     "params": {"around": {"type": "string", "description": "a name, as for `what`; every node when left out"},
                "depth": {"type": "integer", "minimum": 0, "maximum": 3, "default": 1, "description": "how many edges out from --around"}},
     "examples": ["cloud --around Layer --depth 2"], "exits": EXITS, "next": []},
]


def main(argv=None) -> int:
    return _verbs.main(SCRIPT, __doc__, TOOLS, "atlas_", json.loads(SCHEMA.read_text(encoding="utf-8")), argv)


if __name__ == "__main__":
    _verbs.script(SCRIPT, __doc__, TOOLS, "atlas_", json.loads(SCHEMA.read_text(encoding="utf-8")))
