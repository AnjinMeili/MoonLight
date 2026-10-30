"""show_atlas answers a maintainer's question from the graph, so each rule here is how a question is read and which rows answer it.

The graph is small and written for the questions: a base header two includes away from main, a module with its doc section, a test and a scenario, a step that runs a check, two standards sections, and a claim with no home.
"""

import contextlib
import json
import sqlite3

import pytest

import gen_atlas
import show_atlas


@pytest.fixture
def con(tmp_path):
    g = gen_atlas.Graph()
    for p in ("src/core/MoonModule.h", "src/light/Layer.h", "src/light/Layer.cpp", "src/main.cpp", "test/unit/unit_Layer.cpp",
              "test/scenarios/scenario_Layers.json", "docs/page.md", "docs/contributing/coding-standards.md", "moondeck/check/check_x.py"):
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


def test_a_name_means_the_module_and_the_class_it_is_and_a_file_answers_to_its_name_or_stem(con):
    assert show_atlas.resolve(con, "Layer") == ["module:Layer", "class:Layer"]
    assert show_atlas.resolve(con, "src/main.cpp") == ["file:src/main.cpp"]
    assert show_atlas.resolve(con, "MoonModule.h") == ["file:src/core/MoonModule.h"]
    assert show_atlas.resolve(con, "unit_Layer") == ["file:test/unit/unit_Layer.cpp"]


def test_a_name_nothing_holds_is_named_back(con):
    with pytest.raises(LookupError, match="nothing is named Nowhere"):
        show_atlas.resolve(con, "Nowhere")


def test_what_shows_a_node_with_every_edge_leaving_and_arriving(con):
    lines = show_atlas.what(con, "class:Layer")
    assert lines[0] == "class:Layer  src/light/Layer.h:12"
    assert "  derives -> MoonModule  src/core/MoonModule.h:40" in lines
    assert "  is <- module:Layer" in lines
    assert "  mentions <- docs/page.md" in lines


def test_why_gives_the_doc_section_and_the_code_sections_of_the_header_and_its_implementation_by_line(con):
    assert show_atlas.why(con, "Layer") == [
        "documented by (1)", "  docs/moonmodules/light/supporting.md#layer",
        "explained in code (2)", "  src/light/Layer.cpp:8  The status line", "  src/light/Layer.cpp:30  Why one buffer"]


def test_impact_follows_includes_all_the_way_for_tests_and_steps_and_keeps_docs_and_claims_with_the_name(con):
    lines = show_atlas.impact(con, "MoonModule.h")
    assert lines[lines.index("included by (3)"):][:4] == ["included by (3)", "  src/light/Layer.cpp", "  src/light/Layer.h", "  src/main.cpp"]
    assert "  test/unit/unit_Layer.cpp" in lines              # it includes Layer.h, and it names the module Layer.h defines
    assert "  test/scenarios/scenario_Layers.json" in lines[lines.index("scenarios (1)"):]
    assert "  step:first-light/pick  Pick an effect." in lines
    assert "docs: none" in lines and "claims: none" in lines   # the page and the claim are about Layer, not MoonModule


def test_impact_of_a_module_brings_the_pages_that_mention_it_and_the_claims_that_cite_its_code(con):
    lines = show_atlas.impact(con, "Layer")
    assert "  docs/page.md" in lines and "  docs/moonmodules/light/supporting.md#layer" in lines
    assert "  docs/work/present/atlas/pilot/core-x.json#core-001  [partly: One detail is off.] Claim core-001." in lines
    assert not any("core-002" in line for line in lines)      # it cites a page that only mentions Layer


def test_impact_of_a_script_names_the_moonwalk_steps_that_run_it(con):
    assert "  step:commit/x  Run check_x." in show_atlas.impact(con, "check_x.py")


def test_tests_lists_each_test_with_the_steps_it_proves(con):
    assert show_atlas.tests(con, "Layer") == ["test/unit/unit_Layer.cpp", "  proves step:first-light/pick  Pick an effect."]
    assert show_atlas.tests(con, "src/main.cpp") == ["tests: none"]


def test_rules_names_the_check_enforcing_each_section_and_leaves_the_rest_to_review(con):
    assert show_atlas.rules(con) == [
        "docs/contributing/coding-standards.md  <- check_code (the whole page)",
        "  #casts  review",
        "  #naming  <- check_x"]


def test_gaps_groups_the_claims_with_no_home_by_the_page_they_belong_on_less_those_a_review_found_written(con):
    assert show_atlas.gaps(con) == [
        "docs/page.md#layers",
        "  docs/work/present/atlas/pilot/core-x.json#core-001  [partly: One detail is off.] Claim core-001.",
        "    cites src/light/Layer.h [12-14]"]


def test_a_claim_carries_its_review_as_a_prefix_only_when_the_review_found_something(con):
    claims = "claim:docs/work/present/atlas/pilot/core-x.json#"
    assert show_atlas.verdict(con, claims + "core-001") == "[partly: One detail is off.] "
    assert show_atlas.verdict(con, claims + "core-002") == ""
    assert show_atlas.verdict(con, claims + "core-003") == "[home wrong: A comment states it.] "
    assert show_atlas.verdict(con, claims + "core-999") == ""   # never sampled
