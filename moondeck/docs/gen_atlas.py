#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyyaml>=6", "jsonschema>=4", "markdown>=3"]
# ///
# gen_moonwalk's packages, for the MoonWalk model, and the docs build's slugger, for a catalog card named without an anchor.
"""Build MoonAtlas, the repository as a graph a maintainer or an agent queries instead of reading the tree again.

The graph adds no fact of its own: it joins what the repo already holds, so every row points at that fact's one home. A module at its header and its doc section, a test at the modules it pins, a MoonWalk step at the script it runs, a check at the standard it enforces, a claim at the lines it cites, a review at the claims it checked. That makes it a build artifact, SQLite at `build/atlas/atlas.sqlite`, the standard adjacency list of a `nodes` and an `edges` table, rebuilt whenever the tree changed and never committed.

Each source is read by the parser that already owns it: `help.json` for modules and controls, `_test_metadata` for tests, `gen_moonwalk.build` for processes, `gen_moonwalk.anchors_in` for doc anchors, `mkdocs_hooks.catalog_markdown` for the pages the docs build generates, `gen_help` for `@xref`, `check_docgen` for the id of a code section, which a developer reads as text and no page renders, and Python's own `ast` for a script's imports.

Whatever reads the graph refreshes it first, show_atlas and check_atlas alike, so nothing else runs it, and an answer describes the tree as it is or says on stderr that it comes from the last graph.

Usage:
    uv run moondeck/docs/gen_atlas.py            # rebuild when the tree changed since the last build
    uv run moondeck/docs/gen_atlas.py --force    # rebuild regardless
"""

import argparse
import ast
import contextlib
import hashlib
import json
import os
import re
import sqlite3
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(ROOT / "moondeck" / "check"))
sys.path.insert(0, str(ROOT / "moondeck" / "docs"))

import _test_metadata  # noqa: E402
from check_docgen import _CODE_HEADING_RE, _slugify_heading  # noqa: E402
from gen_help import _XREF_RE  # noqa: E402
from gen_moonwalk import _run_lines, anchors_in  # noqa: E402
from mkdocs_hooks import catalog_markdown  # noqa: E402

DB = ROOT / "build" / "atlas" / "atlas.sqlite"
HELP = "docs/help/help.json"
SITE_SOURCE = "https://github.com/MoonModules/MoonLight/blob/main/"
GENERATED = "generated from help.json by the docs build"   # the note on a page the docs build writes, which has no text of its own on disk

SCHEMA = """
CREATE TABLE nodes (id TEXT PRIMARY KEY, kind TEXT NOT NULL, name TEXT, path TEXT, line INTEGER, note TEXT);
CREATE TABLE edges (src TEXT NOT NULL, rel TEXT NOT NULL, dst TEXT NOT NULL, detail TEXT);
CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);
CREATE INDEX edges_src ON edges (src, rel);
CREATE INDEX edges_dst ON edges (dst, rel);
"""

# What each kind of node and each relation means, the one home a reader plans a query with: show_atlas's `schema` verb lists them with their counts, and its --kind and --rel take these names.
KINDS = {
    "file": "A file in the tree: code, a script, a doc page, a test.",
    "class": "A C++ class or struct, at the line its header defines it.",
    "module": "A registered MoonModule, as help.json lists it, with its one-line help.",
    "control": "A module's control, a setting its card shows.",
    "section": "A heading of a doc page, or a `/// ##` section of a code file.",
    "process": "A MoonWalk process, from a stage file under moonwalk/.",
    "step": "One step of a MoonWalk process, with what it says to do.",
    "claim": "A sentence a researcher wrote about the tree, citing the lines that hold it; its review says how far to trust it.",
}
RELS = {
    "includes": "A code file #includes another.",
    "imports": "A Python file imports another.",
    "defines": "A header defines a class, at the line given.",
    "derives": "A class derives from a base class.",
    "in": "A section lies in its file; a step in its process.",
    "xrefs": "A file refers to one of its own sections by anchor, at the line given.",
    "tests": "A test pins a module, a class or a script.",
    "proves": "A test proves a MoonWalk step works.",
    "mentions": "A doc page names a class in backticks.",
    "is": "A module is a class.",
    "defined_in": "A module is defined in its header.",
    "documented_by": "A module, a process or a script is documented by a doc section.",
    "has_control": "A module has a control.",
    "runs": "A step runs a script, with its arguments, or another process.",
    "enforces": "A check enforces a standards section or a whole standards page.",
    "cites": "A claim cites the lines or the section that hold its fact.",
    "reviews": "A review file holds a verdict on a claim.",
}

CODE = (".h", ".hpp", ".c", ".cpp")
PYTEST_DIR = "test/python/"   # the Python unit tests, the folder test_host --python hands pytest, which collects its test_*.py
_INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)
# A class or struct definition, not a declaration: its optional base list runs up to the opening brace.
_CLASS_RE = re.compile(r"^\s*(?:template\s*<[^;{]*?>\s*)?(?:class|struct)\s+(\w+)\s*(?:final\s*)?(?::\s*([^;{]*))?\{", re.MULTILINE)
_STANDARD_RE = re.compile(r"docs/contributing/([\w-]+-standards\.md)(?:#([\w-]+))?")
_TICKED_RE = re.compile(r"`(\w+)`")


def tree_files() -> list[str]:
    """The working tree as its developer sees it: tracked files plus untracked ones git does not ignore, those that exist."""
    out = subprocess.run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                         cwd=ROOT, capture_output=True, text=True, check=True).stdout
    return sorted({p for p in out.split("\0") if p and (ROOT / p).is_file()})


def fingerprint(files: list[str]) -> str:
    """One digest over every file's path, size and modification time: a stat pass, so an unchanged tree costs no read."""
    h = hashlib.sha1()
    for p in files:
        st = (ROOT / p).stat()
        h.update(f"{p}\0{st.st_size}\0{st.st_mtime_ns}\n".encode())
    return h.hexdigest()


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8", errors="replace")


def doc(path: str, anchor: str = "") -> str:
    """A link's node: the section its anchor names, or the page itself."""
    return f"doc:{path}#{anchor}" if anchor else f"file:{path}"


class Graph:
    """The rows of one build. A node seen twice keeps its first description; an edge may point at a node no source defined, and stays in the graph."""

    def __init__(self):
        self.nodes: dict[str, tuple] = {}
        self.edges: list[tuple] = []
        self.skipped: list[str] = []

    def node(self, id_: str, kind: str, name: str = None, path: str = None, line: int = None, note: str = None) -> str:
        self.nodes.setdefault(id_, (id_, kind, name, path, line, note))
        return id_

    def edge(self, src: str, rel: str, dst: str, detail: str = None):
        self.edges.append((src, rel, dst, detail))


def resolver(files: list[str], extensions: tuple = CODE):
    """An `#include "x"` resolves the way the compiler's search finds it in this tree: beside the includer first, else any file whose path ends in x. A Python import resolves the same way, its folder on `sys.path` being the importer's own or one a script inserts."""
    by_suffix: dict[str, list[str]] = {}
    for p in files:
        if p.endswith(extensions):
            parts = p.split("/")
            for i in range(len(parts)):
                by_suffix.setdefault("/".join(parts[i:]), []).append(p)

    def resolve(includer: str, name: str) -> list[str]:
        beside = os.path.normpath(os.path.join(os.path.dirname(includer), name))
        return [beside] if beside in by_suffix.get(name.split("/")[-1], []) else by_suffix.get(name, [])
    return resolve


def base_names(bases: str) -> list[str]:
    """`public A, private B<C, D>` to [A, B]: drop comments, split at the top-level commas, drop access and virtual, keep the name before its template arguments."""
    names, depth, cur = [], 0, ""
    for ch in re.sub(r"//[^\n]*|/\*.*?\*/", " ", bases, flags=re.DOTALL) + ",":
        depth += ch == "<"
        depth -= ch == ">"
        if ch == "," and depth == 0:
            words = [w for w in re.split(r"\s+", re.sub(r"<.*", "", cur).strip()) if w not in ("", "public", "protected", "private", "virtual", "class", "struct", "typename")]
            if words and re.fullmatch(r"[\w:]+", words[-1]):
                names.append(words[-1].split("::")[-1])
            cur = ""
        else:
            cur += ch
    return names


def add_code(g: Graph, files: list[str]):
    """Includes, class definitions with their bases, `/// ## ` sections and the `@xref`s that name them."""
    resolve = resolver(files)
    for p in files:
        if not p.endswith(CODE):
            continue
        text = read(p)
        for name in _INCLUDE_RE.findall(text):
            for target in resolve(p, name):
                g.edge(f"file:{p}", "includes", f"file:{target}")
        for m in _CLASS_RE.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            cls = g.node(f"class:{m[1]}", "class", m[1], p, line)
            g.edge(f"file:{p}", "defines", cls, str(line))
            for base in base_names(m[2] or ""):
                g.edge(cls, "derives", f"class:{base}")
        for i, line in enumerate(text.splitlines(), 1):
            sec = _CODE_HEADING_RE.match(line)
            if sec:
                title = sec[1].strip()
                g.edge(g.node(f"doc:{p}#{_slugify_heading(title)}", "section", title, p, i), "in", f"file:{p}")
        for m in _XREF_RE.finditer(text):
            g.edge(f"file:{p}", "xrefs", f"doc:{p}#{m[1]}", str(text.count("\n", 0, m.start()) + 1))


def python_modules(text: str) -> list[str]:
    """Every module a Python file imports, at any depth, inside a function too: `import a.b` and `from a.b import c` give `a.b`, and `from . import c` gives `c`."""
    found = []
    for n in ast.walk(ast.parse(text)):
        if isinstance(n, ast.Import):
            found += [a.name for a in n.names]
        elif isinstance(n, ast.ImportFrom):
            found += [n.module] if n.module else [a.name for a in n.names]
    return found


def add_python(g: Graph, files: list[str]):
    """Each import of a module in the tree, and a pytest file tests what it imports; a module outside the tree, the standard library or a package, adds nothing. A relative import resolves as an absolute one does, beside the importer first."""
    resolve = resolver(files, (".py",))
    for p in files:
        if not p.endswith(".py"):
            continue
        try:
            modules = python_modules(read(p))
        except (SyntaxError, ValueError) as e:
            print(f"MoonAtlas: {p} skipped, not Python it can parse: {e!r}", file=sys.stderr)
            continue
        is_test = p.startswith(PYTEST_DIR) and Path(p).name.startswith("test_")
        for module in dict.fromkeys(modules):
            path = module.replace(".", "/")
            for target in resolve(p, f"{path}.py") or resolve(p, f"{path}/__init__.py"):
                g.edge(f"file:{p}", "imports", f"file:{target}")
                if is_test:
                    g.edge(f"file:{p}", "tests", f"file:{target}")


def add_docs(g: Graph, files: list[str], names: set[str]):
    """Every Markdown heading as the docs build renders its anchor, and every ticked name a page mentions that is a class or module."""
    for p in files:
        if not p.endswith(".md"):
            continue
        text = read(p)   # bytes that are not UTF-8 read as replacement characters, so one page cannot stop the rest
        for anchor in anchors_in(text):
            g.edge(g.node(f"doc:{p}#{anchor}", "section", anchor, p), "in", f"file:{p}")
        for name in sorted(set(_TICKED_RE.findall(text)) & names):
            g.edge(f"file:{p}", "mentions", f"class:{name}")


def add_catalogs(g: Graph, help_: dict):
    """The module pages the docs build generates from help.json: a page, and an anchor per card. A page with a written intro keeps its file's node and is the docs build's all the same."""
    pages = set(help_.get("catalogs", {})) | {m["doc"].partition("#")[0] for m in help_.get("modules", {}).values() if m.get("doc")}
    for page in sorted(pages):
        path = f"docs/moonmodules/{page}"
        text = catalog_markdown(f"moonmodules/{page}")
        if text is None:
            continue
        fid = g.node(f"file:{path}", "file", page.rsplit("/", 1)[-1], path)
        g.nodes[fid] = g.nodes[fid][:5] + (GENERATED,)
        for anchor in anchors_in(text):
            g.edge(g.node(f"doc:{path}#{anchor}", "section", anchor, path), "in", f"file:{path}")


def add_modules(g: Graph, help_: dict):
    """help.json: each registered module, the header that defines it, its doc section and its controls."""
    for name, m in help_.get("modules", {}).items():
        mod = g.node(f"module:{name}", "module", name, note=m.get("line"))
        g.edge(mod, "is", f"class:{name}")
        if m.get("source", "").startswith(SITE_SOURCE):
            g.edge(mod, "defined_in", f"file:{m['source'][len(SITE_SOURCE):]}")
        if m.get("doc"):
            page, _, anchor = m["doc"].partition("#")
            g.edge(mod, "documented_by", doc(f"docs/moonmodules/{page}", anchor))
        for control, c in (m.get("controls") or {}).items():
            g.edge(mod, "has_control", g.node(f"control:{name}.{control}", "control", control, note=c.get("line")))


def add_tests(g: Graph):
    """Each unit test file and scenario, the modules it names and the MoonWalk steps it proves."""
    for t in _test_metadata.collect_unit_files():
        f = f"file:{Path(t['path']).relative_to(ROOT)}"
        for mod in [t["module"], *t["also"]]:
            if mod:
                g.edge(f, "tests", f"module:{mod}")
        for step in {s for steps in t["case_steps"] for s in steps}:
            g.edge(f, "proves", f"step:{step}")
    for s in _test_metadata.collect_scenario_files():
        f = f"file:{Path(s['path']).relative_to(ROOT)}"
        for mod in [s.get("module"), *s.get("also", [])]:
            if mod:
                g.edge(f, "tests", f"module:{mod}")
        for step in s.get("steps_proved", []):
            g.edge(f, "proves", f"step:{step}")


def add_processes(g: Graph):
    """MoonWalk: each process, its page, its steps, and the scripts a step runs."""
    import gen_moonwalk
    model = gen_moonwalk.build()
    for pid, p in model["processes"].items():
        proc = g.node(f"process:{pid}", "process", p.get("title"), note=p.get("goal"))
        if p.get("page"):
            page, _, anchor = f"docs/{p['page']}".partition("#")
            g.edge(proc, "documented_by", doc(page, anchor))
        for st in p.get("steps", []):
            step = g.node(f"step:{pid}/{st['id']}", "step", st.get("say"))
            g.edge(step, "in", proc)
            run = (st.get("when") or {}).get("run")
            for script, args in _run_lines(run):
                g.edge(step, "runs", f"file:{script}", args.strip() or None)
            sub = (st.get("when") or {}).get("process")
            if sub:
                g.edge(step, "runs", f"process:{sub}")


def add_tools(g: Graph, files: list[str]):
    """MoonDeck: each script's card, and each check's standard, from the links the card and the script carry."""
    config = json.loads(read("moondeck/moondeck_config.json"))
    for s in config.get("scripts", []):
        if s.get("script") and s.get("help"):
            g.edge(f"file:moondeck/{s['script']}", "documented_by", f"doc:moondeck/MoonDeck.md#{s['help']}")
    for p in files:
        if p.startswith("moondeck/check/") and p.endswith(".py"):
            for page, anchor in set(_STANDARD_RE.findall(read(p))):
                g.edge(f"file:{p}", "enforces", doc(f"docs/contributing/{page}", anchor))


def add_claims(g: Graph, files: list[str]):
    """Researcher claims: each one, what it cites, and the page a claim with no written home belongs on."""
    for p in files:
        if "/atlas/" not in p or not p.endswith(".json") or not p.startswith("docs/work/"):
            continue
        try:
            data = json.loads(read(p))
            if not isinstance(data, dict) or not {"area", "model", "claims"} <= data.keys():
                continue  # another atlas file, such as a review, rather than claims
            for c in data["claims"]:
                claim = g.node(f"claim:{p}#{c['id']}", "claim", c["text"], p,
                               note=json.dumps({k: c.get(k) for k in ("subject", "kind", "home", "gap_home")}))
                for cite in c["cites"]:
                    path, _, anchor = cite.partition("#")
                    path, _, lines = path.partition(":")
                    g.edge(claim, "cites", doc(path, anchor), lines or None)
        except (ValueError, KeyError, TypeError) as e:
            print(f"MoonAtlas: {p} skipped, not a claims file: {e!r}", file=sys.stderr)


def reviewed_claim(review: str, entry: dict) -> str:
    """The claim a review entry checked, which a review names by its claims file in the same folder and its id."""
    return f"claim:{review.rsplit('/', 1)[0]}/{entry['file']}.json#{entry['id']}"


def review_order(path: str) -> tuple:
    """A review file's place in its folder: its number, `review.json` being the first, ahead of any numbered one."""
    m = re.search(r"/review(?:-(\d+))?\.json$", path)
    return (path.rsplit("/", 1)[0], int(m[1] or 0) if m else -1, path)


def add_reviews(g: Graph, files: list[str]):
    """Blind reviews: each verdict joined onto the claim it checked. The claim keeps the researcher's words; the verdict keeps its own home.

    Reviews join in the order they ran, `review.json` first and then `review-2.json`, `review-3.json` and on, so the latest edge on a claim is its latest review."""
    for p in sorted(files, key=review_order):
        if "/atlas/" not in p or not p.endswith(".json") or not p.startswith("docs/work/"):
            continue
        try:
            data = json.loads(read(p))
            entries = data.get("claims", []) if isinstance(data, dict) else []
            if not entries or not isinstance(entries[0], dict) or "verdict" not in entries[0]:
                continue  # a claims file, or another atlas file, rather than a review
            for c in entries:
                g.edge(f"file:{p}", "reviews", reviewed_claim(p, c),
                       json.dumps({k: c.get(k) for k in ("verdict", "cites_support", "home_ok", "reason")}))
        except (ValueError, KeyError, TypeError) as e:
            print(f"MoonAtlas: {p} skipped, a review entry it cannot read: {e!r}", file=sys.stderr)


def resolve_names(g: Graph):
    """A test's `@module` names what it pins: a registered module, else a class, else a file by its stem, preferring the one under src/."""
    stems: dict[str, str] = {}
    for row in sorted((r for r in g.nodes.values() if r[1] == "file"), key=lambda r: not r[3].startswith("src/")):
        stems.setdefault(Path(row[3]).stem, row[0])
    for i, (src, rel, dst, detail) in enumerate(g.edges):
        if dst.startswith("module:") and dst not in g.nodes:
            name = dst.split(":", 1)[1]
            alt = f"class:{name}" if f"class:{name}" in g.nodes else stems.get(name)
            if alt:
                g.edges[i] = (src, rel, alt, detail)


def class_names(g: Graph) -> set[str]:
    """The names a page may tick that the graph knows as a class or a module."""
    return {n.split(":", 1)[1] for n, row in g.nodes.items() if row[1] in ("class", "module")}


def build(files: list[str]) -> Graph:
    """Every extractor in turn, under one guard: a source that fails to read is named and skipped, and the rest of the graph stands; `g.skipped` lists what was."""
    g = Graph()
    for p in files:
        g.node(f"file:{p}", "file", p.rsplit("/", 1)[-1], p)
    help_: dict = {}
    stages = (
        ("help.json", lambda: help_.update(json.loads(read(HELP)))),
        ("code", lambda: add_code(g, files)),
        ("python", lambda: add_python(g, files)),
        ("catalogs", lambda: add_catalogs(g, help_)),
        ("modules", lambda: add_modules(g, help_)),
        ("docs", lambda: add_docs(g, files, class_names(g))),
        ("tests", lambda: add_tests(g)),
        ("tools", lambda: add_tools(g, files)),
        ("claims", lambda: add_claims(g, files)),
        ("reviews", lambda: add_reviews(g, files)),
        ("processes", lambda: add_processes(g)),
        ("names", lambda: resolve_names(g)),
    )
    for name, stage in stages:
        try:
            stage()
        except Exception as e:  # each source has its own tool to report it in full; here it degrades to a named skip
            print(f"MoonAtlas: {name} skipped: {e!r}", file=sys.stderr)
            g.skipped.append(name)
    return g


def write(g: Graph, meta: dict, db: Path = DB):
    """Into a fresh file of its own beside the old one, then one rename, so a reader never sees half a graph and two queries refreshing at once never share a file."""
    db.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(suffix=".tmp", prefix=db.name + ".", dir=db.parent)
    os.close(fd)
    tmp = Path(name)
    try:
        # closing(), not the connection's own `with`, which commits but leaves the file open, and Windows refuses to replace an open file.
        with contextlib.closing(sqlite3.connect(tmp)) as con:
            con.executescript(SCHEMA)
            con.executemany("INSERT INTO nodes VALUES (?,?,?,?,?,?)", g.nodes.values())
            con.executemany("INSERT INTO edges VALUES (?,?,?,?)", g.edges)
            con.executemany("INSERT INTO meta VALUES (?,?)", meta.items())
            con.commit()
        os.replace(tmp, db)
    finally:
        tmp.unlink(missing_ok=True)


def stored_fingerprint(db: Path = DB) -> str | None:
    if not db.exists():
        return None
    try:
        with contextlib.closing(sqlite3.connect(db)) as con:
            row = con.execute("SELECT value FROM meta WHERE key = 'fingerprint'").fetchone()
        return row[0] if row else None
    except sqlite3.Error:
        return None


def refresh(force: bool = False) -> str:
    """Bring the graph up to date with the tree and say what that took: one stat pass when nothing changed, the whole graph when anything did."""
    start = time.monotonic()
    files = tree_files()
    fp = fingerprint(files)
    if not force and fp == stored_fingerprint():
        return f"MoonAtlas: unchanged ({time.monotonic() - start:.2f}s)"
    g = build(files)
    head = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    seconds = time.monotonic() - start
    # A partial graph keeps no fingerprint, so the next run rebuilds it rather than calling it current.
    write(g, {"fingerprint": "" if g.skipped else fp, "skipped": ",".join(g.skipped), "commit": head, "seconds": f"{seconds:.2f}"})
    kinds: dict[str, int] = {}
    for row in g.nodes.values():
        kinds[row[1]] = kinds.get(row[1], 0) + 1
    return (f"MoonAtlas: {len(g.nodes)} nodes, {len(g.edges)} edges in {seconds:.2f}s, {DB.relative_to(ROOT)}\n"
            + "  " + ", ".join(f"{k} {n}" for k, n in sorted(kinds.items())))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--force", action="store_true", help="rebuild even when the tree is unchanged")
    print(refresh(ap.parse_args().force))
    return 0


if __name__ == "__main__":
    sys.exit(main())
