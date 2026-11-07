"""show_atlas answers a maintainer's question from the graph, so each rule here is how a question is read and which rows answer it.

The graph is small and written for the questions: a base header two includes away from main, a module with its doc section, a test and a scenario, a step that runs a check, two standards sections, and a claim with no home.
"""

import contextlib
import json
import sqlite3
import sys
from pathlib import Path

import pytest

import gen_atlas
import show_atlas


@pytest.fixture
def con(tmp_path):
    g = gen_atlas.Graph()
    for p in ("src/core/MoonModule.h", "src/light/Layer.h", "src/light/Layer.cpp", "src/main.cpp", "test/unit/unit_Layer.cpp",
              "test/scenarios/scenario_Layers.json", "docs/page.md", "docs/contributing/coding-standards.md", "moondeck/check/check_x.py",
              "moondeck/check/_findings.py", "test/python/test_check_x.py"):
        g.node(f"file:{p}", "file", p.rsplit("/", 1)[-1], p)
    g.edge("file:src/core/MoonModule.h", "defines", g.node("class:MoonModule", "class", "MoonModule", "src/core/MoonModule.h", 40))
    g.edge("file:src/light/Layer.h", "defines", g.node("class:Layer", "class", "Layer", "src/light/Layer.h", 12))
    g.edge("class:Layer", "derives", "class:MoonModule")
    g.edge("file:src/light/Layer.h", "includes", "file:src/core/MoonModule.h")
    g.edge("file:src/light/Layer.cpp", "includes", "file:src/light/Layer.h")
    g.edge("file:src/main.cpp", "includes", "file:src/light/Layer.h")
    g.edge("file:test/unit/unit_Layer.cpp", "includes", "file:src/light/Layer.h")
    for line, title in ((30, "Why one buffer"), (8, "The status line")):
        sec = g.node(f"doc:src/light/Layer.cpp#{title.lower().replace(' ', '-')}", "section", title, "src/light/Layer.cpp", line)
        g.edge(sec, "in", "file:src/light/Layer.cpp")
    g.edge("file:src/light/Layer.cpp", "xrefs", "doc:src/light/Layer.cpp#why-one-buffer", "3")
    mod = g.node("module:Layer", "module", "Layer", note="One rendering layer.")
    g.edge(mod, "is", "class:Layer")
    g.edge(mod, "defined_in", "file:src/light/Layer.h")
    # The card's anchor on a catalog page the docs build generates, as add_catalogs gives it.
    g.node("doc:docs/moonmodules/light/supporting.md#layer", "section", "layer", "docs/moonmodules/light/supporting.md")
    g.edge(mod, "documented_by", "doc:docs/moonmodules/light/supporting.md#layer")
    g.edge("file:test/unit/unit_Layer.cpp", "tests", "module:Layer")
    g.edge("file:test/unit/unit_Layer.cpp", "proves", "step:first-light/pick")
    g.edge("file:test/scenarios/scenario_Layers.json", "tests", "class:MoonModule")
    g.node("step:first-light/pick", "step", "Pick an effect.")
    g.edge(g.node("step:commit/x", "step", "Run check_x."), "runs", "file:moondeck/check/check_x.py")
    g.edge("file:moondeck/check/check_x.py", "imports", "file:moondeck/check/_findings.py")
    g.edge("file:test/python/test_check_x.py", "imports", "file:moondeck/check/check_x.py")
    g.edge("file:test/python/test_check_x.py", "tests", "file:moondeck/check/check_x.py")
    g.edge("file:docs/page.md", "mentions", "class:Layer")
    for anchor in ("naming", "casts"):
        g.edge(g.node(f"doc:docs/contributing/coding-standards.md#{anchor}", "section", anchor, "docs/contributing/coding-standards.md"),
               "in", "file:docs/contributing/coding-standards.md")
    g.edge("file:moondeck/check/check_x.py", "enforces", "doc:docs/contributing/coding-standards.md#naming")
    g.edge("file:moondeck/check/check_code.py", "enforces", "file:docs/contributing/coding-standards.md")
    for cid, home, cite in (("core-001", {"home": "none", "gap_home": "docs/page.md#layers"}, "file:src/light/Layer.h"),
                            ("core-002", {"home": "doc"}, "file:docs/page.md"),
                            ("core-003", {"home": "none", "gap_home": "docs/page.md#layers"}, "file:src/main.cpp")):
        claim = g.node(f"claim:docs/work/present/atlas/pilot/core-x.json#{cid}", "claim", f"Claim {cid}.", "docs/work/present/atlas/pilot/core-x.json",
                       note=json.dumps(home))
        g.edge(claim, "cites", cite, "12-14")
    for cid, found in (("core-001", {"verdict": "partly", "cites_support": True, "home_ok": True, "reason": "One detail is off."}),
                       ("core-002", {"verdict": "correct", "cites_support": True, "home_ok": True, "reason": "As cited."}),
                       ("core-003", {"verdict": "correct", "cites_support": True, "home_ok": False, "reason": "A comment states it."})):
        g.edge("file:docs/work/present/atlas/pilot/review.json", "reviews", f"claim:docs/work/present/atlas/pilot/core-x.json#{cid}", json.dumps(found))
    db = tmp_path / "atlas.sqlite"
    gen_atlas.write(g, {"fingerprint": "x"}, db)
    with contextlib.closing(sqlite3.connect(db)) as c:
        yield c


def test_a_failed_refresh_answers_from_the_last_graph_and_says_so_and_no_graph_is_said_plainly(con, tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(gen_atlas, "refresh", lambda: (_ for _ in ()).throw(OSError("disk full")))
    monkeypatch.setattr(gen_atlas, "DB", tmp_path / "atlas.sqlite")
    monkeypatch.setattr(sys, "argv", ["show_atlas.py", "tests", "Layer"])
    assert show_atlas.main() == 0
    out = capsys.readouterr()
    assert "not refreshed: OSError('disk full'); answering from the last graph" in out.err and "test/unit/unit_Layer.cpp" in out.out
    monkeypatch.setattr(gen_atlas, "DB", tmp_path / "never-built.sqlite")
    assert show_atlas.main() == 1
    assert "no graph to answer from" in capsys.readouterr().err


def test_a_name_means_the_module_and_the_class_it_is_and_a_file_answers_to_its_name_or_stem(con):
    assert show_atlas.resolve(con, "Layer") == ["module:Layer", "class:Layer"]
    assert show_atlas.resolve(con, "src/main.cpp") == ["file:src/main.cpp"]
    assert show_atlas.resolve(con, "MoonModule.h") == ["file:src/core/MoonModule.h"]
    assert show_atlas.resolve(con, "unit_Layer") == ["file:test/unit/unit_Layer.cpp"]


def test_a_name_nothing_holds_is_named_back(con):
    with pytest.raises(LookupError, match="nothing is named Nowhere"):
        show_atlas.resolve(con, "Nowhere")


def test_what_shows_a_node_with_every_edge_leaving_and_arriving(con):
    lines = show_atlas.text(show_atlas.what(con, "class:Layer"))
    assert lines[0] == "class:Layer  src/light/Layer.h:12"
    assert "  derives -> MoonModule  src/core/MoonModule.h:40" in lines
    assert "  is <- module:Layer" in lines
    assert "  mentions <- docs/page.md" in lines


def test_why_gives_the_doc_section_and_the_code_sections_of_the_header_and_its_implementation_by_line(con):
    assert show_atlas.text(show_atlas.why(con, "Layer")) == [
        "documented by (1)", "  docs/moonmodules/light/supporting.md#layer",
        "explained in code (2)", "  src/light/Layer.cpp:8  The status line", "  src/light/Layer.cpp:30  Why one buffer"]


def test_impact_follows_includes_all_the_way_for_tests_and_steps_and_keeps_docs_and_claims_with_the_name(con):
    lines = show_atlas.text(show_atlas.impact(con, "MoonModule.h"))
    assert lines[lines.index("included or imported by (3)"):][:4] == ["included or imported by (3)", "  src/light/Layer.cpp", "  src/light/Layer.h", "  src/main.cpp"]
    assert "  test/unit/unit_Layer.cpp" in lines              # it includes Layer.h, and it names the module Layer.h defines
    assert "  test/scenarios/scenario_Layers.json" in lines[lines.index("scenarios (1)"):]
    assert "  step:first-light/pick  Pick an effect." in lines
    assert "docs: none" in lines and "claims: none" in lines   # the page and the claim are about Layer, not MoonModule


def test_impact_of_a_module_brings_the_pages_that_mention_it_and_the_claims_that_cite_its_code(con):
    lines = show_atlas.text(show_atlas.impact(con, "Layer"))
    assert "  docs/page.md" in lines and "  docs/moonmodules/light/supporting.md#layer" in lines
    assert "  docs/work/present/atlas/pilot/core-x.json#core-001  [partly: One detail is off.] Claim core-001." in lines
    assert not any("core-002" in line for line in lines)      # it cites a page that only mentions Layer


def test_impact_of_a_script_names_the_moonwalk_steps_that_run_it(con):
    assert "  step:commit/x  Run check_x." in show_atlas.text(show_atlas.impact(con, "check_x.py"))


def test_impact_of_a_module_a_script_imports_reaches_the_script_the_tests_importing_it_and_the_steps_running_it(con):
    lines = show_atlas.text(show_atlas.impact(con, "_findings.py"))
    assert lines[:2] == ["included or imported by (1)", "  moondeck/check/check_x.py"]
    assert lines[lines.index("unit tests (1)") + 1] == "  test/python/test_check_x.py"
    assert "  step:commit/x  Run check_x." in lines


def test_tests_lists_each_test_with_the_steps_it_proves(con):
    assert show_atlas.text(show_atlas.tests(con, "Layer")) == ["test/unit/unit_Layer.cpp", "  proves step:first-light/pick  Pick an effect."]
    assert show_atlas.text(show_atlas.tests(con, "src/main.cpp")) == ["tests: none"]
    assert show_atlas.text(show_atlas.tests(con, "check_x.py")) == ["test/python/test_check_x.py"]


def test_rules_names_the_check_enforcing_each_section_and_leaves_the_rest_to_review(con):
    assert show_atlas.text(show_atlas.rules(con)) == [
        "docs/contributing/coding-standards.md  <- check_code (the whole page)",
        "  #casts  review",
        "  #naming  <- check_x"]


def test_gaps_groups_the_claims_with_no_home_by_the_page_they_belong_on_less_those_a_review_found_written(con):
    assert show_atlas.text(show_atlas.gaps(con)) == [
        "docs/page.md#layers",
        "  docs/work/present/atlas/pilot/core-x.json#core-001  [partly: One detail is off.] Claim core-001.",
        "    cites src/light/Layer.h [12-14]"]


def test_a_claim_carries_its_review_as_a_prefix_only_when_the_review_found_something(con):
    claims = "claim:docs/work/present/atlas/pilot/core-x.json#"
    assert show_atlas.verdict(con, claims + "core-001") == "[partly: One detail is off.] "
    assert show_atlas.verdict(con, claims + "core-002") == ""
    assert show_atlas.verdict(con, claims + "core-003") == "[home wrong: A comment states it.] "
    assert show_atlas.verdict(con, claims + "core-999") == ""   # never sampled


# Every verb's whole answer, as the text the code gave when the answers became data (step 9 of the plan). A deliberate change to the text changes these lines with it.
ANSWERS = {
    'what Layer': [
        'module:Layer: One rendering layer.',
        '  defined_in -> src/light/Layer.h',
        '  documented_by -> docs/moonmodules/light/supporting.md#layer',
        '  is -> Layer  src/light/Layer.h:12',
        '  tests <- test/unit/unit_Layer.cpp',
        'class:Layer  src/light/Layer.h:12',
        '  derives -> MoonModule  src/core/MoonModule.h:40',
        '  defines <- src/light/Layer.h',
        '  is <- module:Layer',
        '  mentions <- docs/page.md',
    ],
    'what MoonModule.h': [
        'file:src/core/MoonModule.h',
        '  defines -> MoonModule  src/core/MoonModule.h:40',
        '  includes <- src/light/Layer.h',
    ],
    'what step:commit/x': [
        'step:commit/x  Run check_x.',
        '  runs -> moondeck/check/check_x.py',
    ],
    'what claim:docs/work/present/atlas/pilot/core-x.json#core-001': [
        'claim:docs/work/present/atlas/pilot/core-x.json#core-001  Claim core-001.',
        '  cites -> src/light/Layer.h [12-14]',
        '  reviews <- file:docs/work/present/atlas/pilot/review.json (no source defines it) [{"verdict": "partly", "cites_support": true, "home_ok": true, "reason": "One detail is off."}]',
    ],
    'why Layer': [
        'documented by (1)',
        '  docs/moonmodules/light/supporting.md#layer',
        'explained in code (2)',
        '  src/light/Layer.cpp:8  The status line',
        '  src/light/Layer.cpp:30  Why one buffer',
    ],
    'why MoonModule.h': [
        'documented by: none',
        'explained in code: none',
    ],
    'impact MoonModule.h': [
        'included or imported by (3)',
        '  src/light/Layer.cpp',
        '  src/light/Layer.h',
        '  src/main.cpp',
        'unit tests (1)',
        '  test/unit/unit_Layer.cpp',
        'scenarios (1)',
        '  test/scenarios/scenario_Layers.json',
        'MoonWalk steps (1)',
        '  step:first-light/pick  Pick an effect.',
        'docs: none',
        'claims: none',
    ],
    'impact Layer': [
        'included or imported by (1)',
        '  src/main.cpp',
        'unit tests (1)',
        '  test/unit/unit_Layer.cpp',
        'scenarios: none',
        'MoonWalk steps (1)',
        '  step:first-light/pick  Pick an effect.',
        'docs (2)',
        '  docs/moonmodules/light/supporting.md#layer',
        '  docs/page.md',
        'claims (1)',
        '  docs/work/present/atlas/pilot/core-x.json#core-001  [partly: One detail is off.] Claim core-001.',
    ],
    'impact check_x.py': [
        'included or imported by: none',
        'unit tests (1)',
        '  test/python/test_check_x.py',
        'scenarios: none',
        'MoonWalk steps (1)',
        '  step:commit/x  Run check_x.',
        'docs: none',
        'claims: none',
    ],
    'impact _findings.py': [
        'included or imported by (1)',
        '  moondeck/check/check_x.py',
        'unit tests (1)',
        '  test/python/test_check_x.py',
        'scenarios: none',
        'MoonWalk steps (1)',
        '  step:commit/x  Run check_x.',
        'docs: none',
        'claims: none',
    ],
    'impact src/main.cpp': [
        'included or imported by: none',
        'unit tests: none',
        'scenarios: none',
        'MoonWalk steps: none',
        'docs: none',
        'claims (1)',
        '  docs/work/present/atlas/pilot/core-x.json#core-003  [home wrong: A comment states it.] Claim core-003.',
    ],
    'tests Layer': [
        'test/unit/unit_Layer.cpp',
        '  proves step:first-light/pick  Pick an effect.',
    ],
    'tests src/main.cpp': [
        'tests: none',
    ],
    'tests check_x.py': [
        'test/python/test_check_x.py',
    ],
    'rules': [
        'docs/contributing/coding-standards.md  <- check_code (the whole page)',
        '  #casts  review',
        '  #naming  <- check_x',
    ],
    'gaps': [
        'docs/page.md#layers',
        '  docs/work/present/atlas/pilot/core-x.json#core-001  [partly: One detail is off.] Claim core-001.',
        '    cites src/light/Layer.h [12-14]',
    ],
}


def test_every_verb_answers_in_exactly_these_lines_whatever_kind_of_node_it_is_asked_about(con):
    for query, lines in ANSWERS.items():
        verb, _, name = query.partition(" ")
        fn = getattr(show_atlas, verb)
        assert show_atlas.text(fn(con, name) if name else fn(con)) == lines, query


def rows_of(answer):
    """Every row under an answer's groups, the rows nested under a row or under a group's node included."""
    stack = [r for g in answer["groups"] for r in g.get("node", {}).get("rows", []) + g["rows"]]
    while stack:
        r = stack.pop()
        yield r
        stack += r.get("rows", [])


def test_every_answer_is_data_under_its_schema_and_its_text_names_each_row_it_holds(con):
    jsonschema = pytest.importorskip("jsonschema")
    schema = json.loads(show_atlas.SCHEMA.read_text(encoding="utf-8"))
    for query in ANSWERS:
        verb, _, name = query.partition(" ")
        fn = getattr(show_atlas, verb)
        found = fn(con, name) if name else fn(con)
        jsonschema.validate(found, schema)
        assert (found["verb"], found["query"]) == (verb, name or None)
        lines = "\n".join(show_atlas.text(found))
        for g in found["groups"]:
            if "node" in g:   # what heads its group with the node's id, rules with the page
                assert (g["node"]["id"] if verb == "what" else g["title"]) in lines, (query, g["title"])
        for r in rows_of(found):
            shown = Path(r["id"]).stem if r.get("rel") == "enforces" else r["id"].partition("#")[2] if verb == "rules" and r["kind"] == "section" else show_atlas.label(r)
            assert shown in lines, (query, r["id"])


def test_json_prints_the_answer_with_the_graph_it_was_read_from(con, tmp_path, monkeypatch, capsys):
    jsonschema = pytest.importorskip("jsonschema")
    monkeypatch.setattr(gen_atlas, "refresh", lambda: "MoonAtlas: up to date")
    monkeypatch.setattr(gen_atlas, "DB", tmp_path / "atlas.sqlite")
    monkeypatch.setattr(sys, "argv", ["show_atlas.py", "tests", "Layer", "--json"])
    assert show_atlas.main() == 0
    printed = json.loads(capsys.readouterr().out)
    jsonschema.validate(printed, json.loads(show_atlas.SCHEMA.read_text(encoding="utf-8")))
    assert printed["graph"] == {"fresh": True, "note": "MoonAtlas: up to date"}
    assert [r["id"] for r in printed["groups"][0]["rows"]] == ["file:test/unit/unit_Layer.cpp"]
    assert printed["groups"][0]["rows"][0]["rows"][0] == {"id": "step:first-light/pick", "kind": "step", "name": "Pick an effect.", "path": None, "line": None, "rel": "proves", "dir": "out"}
