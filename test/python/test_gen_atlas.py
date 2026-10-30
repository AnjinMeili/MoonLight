"""gen_atlas joins what the repo already holds into one graph, so each rule here is a fact a maintainer or an agent reads back from a query.

An extractor that stops matching leaves no error, only a graph with fewer edges, indistinguishable from a tree with less in it. So each one is pinned firing on a small tree written for it, and staying silent where it should.
"""

import contextlib
import json
import sqlite3
import sys

import pytest

import gen_atlas


@pytest.fixture
def tree(tmp_path, monkeypatch):
    """A repository in a temporary folder: write(path, text) adds a file, and graph() reads the code and docs extractors over it."""
    monkeypatch.setattr(gen_atlas, "ROOT", tmp_path)
    files = []

    def write(path, text):
        (tmp_path / path).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / path).write_text(text, encoding="utf-8")
        files.append(path)

    def graph():
        g = gen_atlas.Graph()
        for p in files:
            g.node(f"file:{p}", "file", p, p)
        gen_atlas.add_code(g, sorted(files))
        gen_atlas.add_python(g, sorted(files))
        names = {n.split(":", 1)[1] for n, row in g.nodes.items() if row[1] == "class"}
        gen_atlas.add_docs(g, sorted(files), names)
        gen_atlas.add_claims(g, sorted(files))
        return g

    write.graph = graph
    return write


def edges(g, rel):
    return {(s, d) for s, r, d, _ in g.edges if r == rel}


def test_an_include_resolves_beside_its_includer_before_anywhere_else(tree):
    tree("src/a/util.h", "")
    tree("src/b/util.h", "")
    tree("src/a/x.cpp", '#include "util.h"\n')
    assert edges(tree.graph(), "includes") == {("file:src/a/x.cpp", "file:src/a/util.h")}


def test_an_include_with_a_directory_resolves_by_path_suffix_and_a_system_include_adds_nothing(tree):
    tree("src/core/module/MoonModule.h", "")
    tree("src/light/Layer.h", '#include "core/module/MoonModule.h"\n#include <vector>\n#include "missing.h"\n')
    assert edges(tree.graph(), "includes") == {("file:src/light/Layer.h", "file:src/core/module/MoonModule.h")}


def test_a_class_records_its_bases_without_access_virtual_or_template_arguments():
    assert gen_atlas.base_names("public MoonModule, private virtual mm::Pool<int, 4>") == ["MoonModule", "Pool"]
    assert gen_atlas.base_names("") == []


def test_a_comment_inside_a_base_list_names_no_base():
    # doctest.h writes a lint marker after a base: its words once became a class named `of`.
    assert gen_atlas.base_names("public IExceptionTranslator //!OCLINT destructor of virtual class\n    ") == ["IExceptionTranslator"]
    assert gen_atlas.base_names("public A /* , B */, public C") == ["A", "C"]


def test_an_import_resolves_beside_its_importer_before_anywhere_else_and_a_module_outside_the_tree_adds_nothing(tree):
    tree("moondeck/check/_findings.py", "")
    tree("moondeck/docs/_findings.py", "")
    tree("moondeck/check/check_x.py", "import json\nimport yaml\nimport _findings  # noqa: E402\n")
    assert edges(tree.graph(), "imports") == {("file:moondeck/check/check_x.py", "file:moondeck/check/_findings.py")}


def test_an_import_inside_a_function_or_of_a_dotted_module_resolves_by_path_suffix(tree):
    tree("moondeck/docs/gen_moonwalk.py", "")
    tree("tools/pkg/util.py", "")
    tree("moondeck/docs/gen_x.py", "from pkg.util import f\n\n\ndef build():\n    import gen_moonwalk\n")
    assert edges(tree.graph(), "imports") == {("file:moondeck/docs/gen_x.py", "file:tools/pkg/util.py"),
                                              ("file:moondeck/docs/gen_x.py", "file:moondeck/docs/gen_moonwalk.py")}


def test_a_pytest_file_tests_what_it_imports_and_another_file_importing_the_same_does_not(tree):
    tree("moondeck/docs/gen_x.py", "")
    tree("test/python/test_gen_x.py", "import gen_x\n")
    tree("test/python/conftest.py", "import gen_x\n")
    tree("moondeck/test/test_host.py", "import gen_x\n")   # a MoonDeck script with pytest's name, outside the folder pytest is handed
    g = tree.graph()
    assert edges(g, "tests") == {("file:test/python/test_gen_x.py", "file:moondeck/docs/gen_x.py")}
    assert len(edges(g, "imports")) == 3


def test_a_python_file_that_does_not_parse_is_named_and_the_others_still_give_their_imports(tree, capsys):
    tree("moondeck/a.py", "def broken(:\n")
    tree("moondeck/b.py", "import c\n")
    tree("moondeck/c.py", "")
    assert edges(tree.graph(), "imports") == {("file:moondeck/b.py", "file:moondeck/c.py")}
    assert "moondeck/a.py skipped" in capsys.readouterr().err


def test_a_class_definition_is_a_node_and_a_forward_declaration_is_not(tree):
    tree("src/Layer.h", "class Effects;\nclass Layer : public MoonModule {\n};\n")
    g = tree.graph()
    assert "class:Layer" in g.nodes and "class:Effects" not in g.nodes
    assert edges(g, "derives") == {("class:Layer", "class:MoonModule")}


def test_an_xref_points_at_a_code_section_of_its_own_file_by_the_id_check_docgen_reads(tree):
    # A code section is read as text and no page renders it, so its id is check_docgen's, where an underscore reads as a space.
    tree("src/main.cpp", "/// Stages a file: @xref{why-write-file-exists}.\n///\n/// ## Why write_file exists\n///\n/// The editor saves this way.\n")
    g = tree.graph()
    assert edges(g, "xrefs") == {("file:src/main.cpp", "doc:src/main.cpp#why-write-file-exists")}
    assert "doc:src/main.cpp#why-write-file-exists" in g.nodes


def test_a_page_mentions_only_the_ticked_names_that_are_classes(tree):
    tree("src/Layer.h", "class Layer : public MoonModule {\n};\n")
    tree("docs/page.md", "# Layers\n\n`Layer` holds a buffer; `buffer` and Layer unticked are words.\n")
    g = tree.graph()
    assert edges(g, "mentions") == {("file:docs/page.md", "class:Layer")}
    assert "doc:docs/page.md#layers" in g.nodes


def test_a_link_lands_on_its_section_or_on_the_page_itself():
    assert gen_atlas.doc("docs/a.md", "intro") == "doc:docs/a.md#intro"
    assert gen_atlas.doc("docs/a.md") == "file:docs/a.md"


def test_a_claim_cites_lines_and_sections_and_a_review_file_is_not_read_as_claims(tree):
    claim = {"id": "core-001", "subject": "Layer", "kind": "what", "text": "A layer holds a buffer.",
             "cites": ["src/Layer.h:2-3", "docs/page.md#layers"], "home": "doc"}
    tree("docs/work/present/atlas/pilot/core-x.json", json.dumps({"area": "core", "model": "x", "scope": [], "claims": [claim]}))
    tree("docs/work/present/atlas/pilot/review.json", json.dumps({"claims": [{"id": "core-001", "verdict": "correct"}]}))
    g = tree.graph()
    cid = "claim:docs/work/present/atlas/pilot/core-x.json#core-001"
    assert {(s, d, detail) for s, r, d, detail in g.edges if r == "cites"} == {
        (cid, "file:src/Layer.h", "2-3"), (cid, "doc:docs/page.md#layers", None)}
    assert [n for n in g.nodes if n.startswith("claim:")] == [cid]


def test_a_test_naming_a_class_or_a_file_pins_that_when_no_module_has_the_name():
    g = gen_atlas.Graph()
    g.node("class:Scheduler", "class", "Scheduler", "src/core/module/Scheduler.h")
    g.node("file:src/light/util/draw.h", "file", "draw.h", "src/light/util/draw.h")
    g.node("module:Layer", "module", "Layer")
    for name in ("Scheduler", "draw", "Layer", "Nowhere"):
        g.edge("file:test/unit_x.cpp", "tests", f"module:{name}")
    gen_atlas.resolve_names(g)
    assert {d for _, _, d, _ in g.edges} == {"class:Scheduler", "file:src/light/util/draw.h", "module:Layer", "module:Nowhere"}


def test_a_name_that_is_a_file_stem_in_several_places_resolves_to_the_one_under_src():
    g = gen_atlas.Graph()
    g.node("file:moondeck/check/platform.h", "file", "platform.h", "moondeck/check/platform.h")
    g.node("file:src/platform/platform.h", "file", "platform.h", "src/platform/platform.h")
    g.edge("file:test/unit_x.cpp", "tests", "module:platform")
    gen_atlas.resolve_names(g)
    assert g.edges == [("file:test/unit_x.cpp", "tests", "file:src/platform/platform.h", None)]


def test_the_graph_is_written_whole_and_remembers_the_tree_it_was_built_from(tmp_path):
    db = tmp_path / "atlas" / "atlas.sqlite"
    g = gen_atlas.Graph()
    g.edge(g.node("file:a.h", "file", "a.h", "a.h"), "includes", "file:b.h")
    gen_atlas.write(g, {"fingerprint": "abc"}, db)
    assert gen_atlas.stored_fingerprint(db) == "abc"
    assert list(db.parent.iterdir()) == [db]   # the writer's own temporary file is gone
    with contextlib.closing(sqlite3.connect(db)) as con:
        assert con.execute("SELECT src, rel, dst FROM edges").fetchall() == [("file:a.h", "includes", "file:b.h")]


def test_each_write_has_a_temporary_file_of_its_own(tmp_path, monkeypatch):
    # Agents querying in parallel each refresh a changed tree: a temporary file per writer keeps one from writing into another's.
    db = tmp_path / "atlas.sqlite"
    staged = []
    real_replace = gen_atlas.os.replace
    monkeypatch.setattr(gen_atlas.os, "replace", lambda src, dst: (staged.append(str(src)), real_replace(src, dst)))
    g = gen_atlas.Graph()
    g.node("file:a.h", "file", "a.h", "a.h")
    gen_atlas.write(g, {"fingerprint": "abc"}, db)
    gen_atlas.write(g, {"fingerprint": "def"}, db)
    assert len(staged) == 2 and staged[0] != staged[1]
    assert gen_atlas.stored_fingerprint(db) == "def"


def test_a_missing_or_unreadable_graph_has_no_fingerprint_so_the_next_run_rebuilds(tmp_path):
    assert gen_atlas.stored_fingerprint(tmp_path / "none.sqlite") is None
    (tmp_path / "junk.sqlite").write_text("not a database")
    assert gen_atlas.stored_fingerprint(tmp_path / "junk.sqlite") is None


HELP = {
    "catalogs": {"light/modifiers.md": {"title": "Modifiers"}},
    "modules": {"Mirror": {"line": "Mirrors the layer.", "doc": "light/modifiers.md#mirror",
                           "source": gen_atlas.SITE_SOURCE + "src/light/modifiers/MirrorModifier.h",
                           "controls": {"axis": {"line": "Which axis mirrors."}}}},
}


def test_help_json_gives_each_module_its_header_its_doc_section_and_its_controls():
    g = gen_atlas.Graph()
    gen_atlas.add_modules(g, HELP)
    assert {(s, r, d) for s, r, d, _ in g.edges} == {
        ("module:Mirror", "is", "class:Mirror"),
        ("module:Mirror", "defined_in", "file:src/light/modifiers/MirrorModifier.h"),
        ("module:Mirror", "documented_by", "doc:docs/moonmodules/light/modifiers.md#mirror"),
        ("module:Mirror", "has_control", "control:Mirror.axis")}


def test_a_page_the_docs_build_generates_is_a_file_node_with_its_card_anchors(monkeypatch):
    monkeypatch.setattr(gen_atlas, "catalog_markdown", lambda uri: "# Modifiers\n\n<a id=\"mirror\"></a>\n" if uri == "moonmodules/light/modifiers.md" else None)
    g = gen_atlas.Graph()
    gen_atlas.add_catalogs(g, HELP)
    assert g.nodes["file:docs/moonmodules/light/modifiers.md"][5] == "generated from help.json by the docs build"
    assert "doc:docs/moonmodules/light/modifiers.md#mirror" in g.nodes


def test_a_test_file_pins_its_modules_and_the_moonwalk_steps_it_proves(tree, tmp_path, monkeypatch):
    unit = {"path": tmp_path / "test/unit/unit_Mirror.cpp", "module": "Mirror", "also": ["Layer"], "case_steps": [[], ["first-light/pick"]]}
    scenario = {"path": str(tmp_path / "test/scenarios/scenario_x.json"), "module": "Layer", "steps_proved": ["first-light/look"]}
    monkeypatch.setattr(gen_atlas._test_metadata, "collect_unit_files", lambda: [unit])
    monkeypatch.setattr(gen_atlas._test_metadata, "collect_scenario_files", lambda: [scenario])
    g = gen_atlas.Graph()
    gen_atlas.add_tests(g)
    assert {(s, r, d) for s, r, d, _ in g.edges} == {
        ("file:test/unit/unit_Mirror.cpp", "tests", "module:Mirror"),
        ("file:test/unit/unit_Mirror.cpp", "tests", "module:Layer"),
        ("file:test/unit/unit_Mirror.cpp", "proves", "step:first-light/pick"),
        ("file:test/scenarios/scenario_x.json", "tests", "module:Layer"),
        ("file:test/scenarios/scenario_x.json", "proves", "step:first-light/look")}


def test_a_script_points_at_its_card_and_a_check_at_the_standard_it_links(tree):
    tree("moondeck/moondeck_config.json", json.dumps({"scripts": [{"script": "docs/gen_x.py", "help": "gen_x"}, {"script": "run/no_card.py"}]}))
    tree("moondeck/check/check_x.py", '"""Enforces docs/contributing/coding-standards.md#naming."""\n')
    tree("moondeck/docs/gen_x.py", '"""Links docs/contributing/coding-standards.md, which only a check enforces."""\n')
    g = gen_atlas.Graph()
    gen_atlas.add_tools(g, ["moondeck/check/check_x.py", "moondeck/docs/gen_x.py"])
    assert {(s, r, d) for s, r, d, _ in g.edges} == {
        ("file:moondeck/docs/gen_x.py", "documented_by", "doc:moondeck/MoonDeck.md#gen_x"),
        ("file:moondeck/check/check_x.py", "enforces", "doc:docs/contributing/coding-standards.md#naming")}


def test_a_moonwalk_step_runs_its_script_or_its_subprocess_and_its_process_links_its_page(monkeypatch):
    import gen_moonwalk
    model = {"processes": {"commit": {"title": "Commit", "page": "moonwalk.md#commit", "steps": [
        {"id": "specs", "say": "Check the specs.", "when": {"run": {"script": "moondeck/check/check_specs.py"}}},
        {"id": "docs", "say": "Run the docs checks.", "when": {"process": "commit-docs"}},
        {"id": "stage", "say": "Stage the diff.", "when": {"manual": "Staged means read."}}]}}}
    monkeypatch.setattr(gen_moonwalk, "build", lambda: model)
    g = gen_atlas.Graph()
    gen_atlas.add_processes(g)
    assert {(s, r, d) for s, r, d, _ in g.edges if r != "in"} == {
        ("process:commit", "documented_by", "doc:docs/moonwalk.md#commit"),
        ("step:commit/specs", "runs", "file:moondeck/check/check_specs.py"),
        ("step:commit/docs", "runs", "process:commit-docs")}


def test_a_review_joins_each_verdict_onto_the_claim_it_checked_in_its_own_folder(tree):
    claim = {"id": "core-001", "subject": "Layer", "kind": "what", "text": "A layer holds a buffer.", "cites": ["src/Layer.h:2"], "home": "none"}
    tree("docs/work/present/atlas/areas/core-x.json", json.dumps({"area": "core", "model": "x", "scope": [], "claims": [claim]}))
    verdict = {"file": "core-x", "id": "core-001", "verdict": "partly", "cites_support": True, "home_ok": False, "reason": "Layer.h:9 says so."}
    tree("docs/work/present/atlas/areas/review.json", json.dumps({"claims": [verdict]}))
    files = ["docs/work/present/atlas/areas/core-x.json", "docs/work/present/atlas/areas/review.json"]
    g = gen_atlas.Graph()
    gen_atlas.add_reviews(g, files)
    assert [(s, r, d) for s, r, d, _ in g.edges] == [
        ("file:docs/work/present/atlas/areas/review.json", "reviews", "claim:docs/work/present/atlas/areas/core-x.json#core-001")]
    assert json.loads(g.edges[0][3]) == {"verdict": "partly", "cites_support": True, "home_ok": False, "reason": "Layer.h:9 says so."}


def test_a_malformed_claims_file_is_named_and_skipped(tree, capsys):
    tree("docs/work/present/atlas/pilot/broken.json", json.dumps({"area": "core", "model": "x", "claims": [{"id": "core-001"}]}))
    g = tree.graph()
    assert not [n for n in g.nodes if n.startswith("claim:")]
    assert "broken.json skipped" in capsys.readouterr().err


def test_a_source_that_fails_to_read_is_named_and_the_rest_of_the_graph_stands(tree, monkeypatch, capsys):
    import gen_moonwalk
    monkeypatch.setattr(gen_moonwalk, "build", lambda: (_ for _ in ()).throw(ValueError("bad stage file")))
    monkeypatch.setattr(gen_atlas._test_metadata, "collect_unit_files", lambda: [])
    monkeypatch.setattr(gen_atlas._test_metadata, "collect_scenario_files", lambda: [])
    tree("src/Layer.h", "class Layer : public MoonModule {\n};\n")
    g = gen_atlas.build(["src/Layer.h"])   # no help.json and no MoonDeck config in this tree
    err = capsys.readouterr().err
    assert "help.json skipped" in err and "tools skipped" in err and "processes skipped" in err
    assert ("class:Layer", "derives", "class:MoonModule") in {(s, r, d) for s, r, d, _ in g.edges}
    assert g.skipped == ["help.json", "tools", "processes"]   # main() then stores no fingerprint, so the next run rebuilds


def test_a_page_with_bytes_that_are_not_utf8_still_gives_its_anchors_and_the_others_theirs(tree, tmp_path):
    tree("docs/good.md", "# Good\n")
    (tmp_path / "docs/bad.md").write_bytes(b"# Bad \xff heading\n\n## Second\n")
    g = gen_atlas.Graph()
    gen_atlas.add_docs(g, ["docs/bad.md", "docs/good.md"], set())
    assert {"doc:docs/good.md#good", "doc:docs/bad.md#second"} <= set(g.nodes)


def test_the_fingerprint_moves_when_a_file_changes_and_holds_when_none_does(tree):
    tree("src/a.h", "int a;\n")
    before = gen_atlas.fingerprint(["src/a.h"])
    assert gen_atlas.fingerprint(["src/a.h"]) == before
    tree("src/a.h", "int a, b;\n")
    assert gen_atlas.fingerprint(["src/a.h"]) != before


def test_a_failed_refresh_is_said_on_stderr_and_exits_0_so_the_build_that_runs_it_stays_green(monkeypatch, capsys):
    monkeypatch.setattr(gen_atlas, "refresh", lambda force=False: (_ for _ in ()).throw(OSError("disk full")))
    monkeypatch.setattr(sys, "argv", ["gen_atlas.py"])
    assert gen_atlas.main() == 0
    assert "MoonAtlas: not refreshed: OSError('disk full')" in capsys.readouterr().err
