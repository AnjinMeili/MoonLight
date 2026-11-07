"""check_atlas keeps MoonAtlas honest: each rule here is a way a row stops pointing at what it says, pinned firing and staying silent.

The graph is written for the rules: a claims file whose claims cite lines, sections and files that exist or do not, a test naming a module that is not one, a review of a claim since removed, and two modules, one with its doc section.
"""

import contextlib
import json
import sqlite3
import subprocess

import pytest

import check_atlas
import gen_atlas

CLAIMS = "docs/work/present/atlas/areas/core-x.json"


@pytest.fixture
def con(tmp_path, monkeypatch):
    monkeypatch.setattr(check_atlas, "ROOT", tmp_path)
    check_atlas.text_of.cache_clear()
    (tmp_path / "src").mkdir()
    (tmp_path / "src/Layer.h").write_text("line 1\nline 2\nline 3\n")
    claims = [{"id": f"core-00{i}"} for i in range(1, 6)]
    (tmp_path / "docs/work/present/atlas/areas").mkdir(parents=True)
    (tmp_path / CLAIMS).write_text(json.dumps({"claims": claims}, indent=1))
    g = gen_atlas.Graph()
    for p in ("src/Layer.h", "docs/page.md", "test/unit/unit_x.cpp", CLAIMS, "docs/work/present/atlas/areas/review.json"):
        g.node(f"file:{p}", "file", p.rsplit("/", 1)[-1], p)
    g.node("doc:docs/page.md#layers", "section", "layers", "docs/page.md")
    for cid, cite, lines in (("core-001", "file:src/Layer.h", "1-3"), ("core-002", "doc:docs/page.md#layers", None),
                             ("core-003", "file:src/Layer.h", "3-4"), ("core-004", "doc:docs/page.md#nosuch", None),
                             ("core-005", "file:src/Gone.h", "1")):
        g.edge(g.node(f"claim:{CLAIMS}#{cid}", "claim", f"Claim {cid}.", CLAIMS), "cites", cite, lines)
    g.edge("file:test/unit/unit_x.cpp", "tests", "module:Nowhere")
    g.edge("file:docs/work/present/atlas/areas/review.json", "reviews", f"claim:{CLAIMS}#core-099")
    g.edge("class:Layer", "derives", "class:LittleFS")
    g.edge(g.node("module:Layer", "module", "Layer"), "documented_by", "doc:docs/page.md#layers")
    g.node("module:Bare", "module", "Bare")
    db = tmp_path / "atlas.sqlite"
    gen_atlas.write(g, {"fingerprint": "x", "skipped": ""}, db)
    with contextlib.closing(sqlite3.connect(db)) as c:
        yield c


def messages(found):
    return [f["message"] for f in found]


def test_a_citation_resolves_when_its_file_anchor_and_lines_exist_and_each_way_it_fails_is_named(con):
    found = check_atlas.unresolved(con)
    assert messages(found) == [
        f"{CLAIMS}#core-003 cites src/Layer.h:3-4 is not within its 3 lines",
        f"{CLAIMS}#core-004 cites docs/page.md has no #nosuch",
        f"{CLAIMS}#core-005 cites src/Gone.h is not in the tree"]
    assert {f["level"] for f in found} == {"error"}
    assert found[0]["file"] == CLAIMS and found[0]["line"] == 10   # the line of the claim's id in its claims file


class History:
    """git's answers, written for each test: each entry's commit, the files changed since a commit, and a file's text then."""

    def __init__(self, entries=None, changed=(), texts=None, order=None):
        self._entries, self._changed, self._texts, self.order = entries or {}, frozenset(changed), texts or {}, order or {}
        self.refused = set()

    def entries(self, path):
        return self._entries.get(path, [])

    def changed(self, commit):
        return self._changed

    def text(self, commit, path):
        t = self._texts.get((commit, path))
        return None if t is None else tuple(t.splitlines())


@pytest.fixture
def now(tmp_path, monkeypatch):
    """write(path, text) puts a file in the tree."""
    monkeypatch.setattr(check_atlas, "ROOT", tmp_path)
    check_atlas.text_of.cache_clear()

    def write(path, text):
        (tmp_path / path).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / path).write_text(text)
    return write


def was(text, path="src/a.h"):
    return History(changed={path}, texts={("base", path): text})


def test_a_citation_whose_text_is_unchanged_is_current_though_its_file_changed_around_it(now):
    now("src/a.h", "x\none\ntwo\nadded below\n")
    assert check_atlas.state("src/a.h", "2-3", "base", was("x\none\ntwo\n")) == ("current", "", None)
    assert check_atlas.state("src/a.h", "2-3", "base", History()) == ("current", "", None)   # no commit since touched the file


def test_a_citation_whose_text_sits_once_elsewhere_has_moved_and_the_finding_names_its_new_lines(now):
    now("src/a.h", "added above\nx\nint firstLine = citedOnce();\nint secondLine = citedTwice();\n")
    assert check_atlas.state("src/a.h", "2-3", "base", was("x\nint firstLine = citedOnce();\nint secondLine = citedTwice();\n")) == ("moved", "src/a.h:2-3 is now 3-4", "3-4")


def test_a_block_under_gits_twenty_letters_and_digits_found_once_elsewhere_is_changed_not_moved(now):
    now("src/a.h", "added\nx\nreturn ok;\n")
    assert check_atlas.state("src/a.h", "2", "base", was("x\nreturn ok;\n")) == ("changed", "src/a.h:2 changed", None)   # 8 letters: perhaps another line
    now("src/a.h", "added\nx\nreturn okayAfterAllOfIt;\n")
    check_atlas.text_of.cache_clear()
    assert check_atlas.state("src/a.h", "2", "base", was("x\nreturn okayAfterAllOfIt;\n"))[0] == "moved"   # 22 letters: the same line


def test_a_citation_whose_text_differs_repeats_or_was_never_there_makes_its_claim_stale(now):
    now("src/a.h", "}\nx\nONE\n}\n")
    assert check_atlas.state("src/a.h", "3", "base", was("x\n}\none\n")) == ("changed", "src/a.h:3 changed", None)
    assert check_atlas.state("src/a.h", "2", "base", was("x\n}\none\n")) == ("changed", "src/a.h:2 changed", None)   # `}` is twice in the file now
    assert check_atlas.state("src/a.h", "4-5", "base", was("x\n")) == ("changed", "src/a.h:4-5 was past the end in the claim's commit", None)
    assert check_atlas.state("src/a.h", "1", "base", History(changed={"src/a.h"})) == ("changed", "src/a.h is not in the claim's commit", None)


def catalog(blur, intro="Modifiers reshape a layer."):
    """help.json for one catalog page of two cards, its Blur card's line and the page's intro as given."""
    return json.dumps({"catalogs": {"light/modifiers.md": {"line": intro}},
                       "modules": {"Mirror": {"doc": "light/modifiers.md#mirror", "line": "Mirrors."}, "Blur": {"doc": "light/modifiers.md#blur", "line": blur}}})


def test_a_catalog_card_has_no_text_on_disk_so_it_is_compared_through_its_modules_entry_in_help_json(now):
    page, help_ = "docs/moonmodules/light/modifiers.md", "docs/help/help.json"
    now(help_, catalog("Blurs more."))
    history = History(changed={help_}, texts={("base", help_): catalog("Blurs.")})
    generated = frozenset({page})
    assert check_atlas.state(f"{page}#mirror", None, "base", history, generated) == ("current", "", None)
    assert check_atlas.state(f"{page}#blur", None, "base", history, generated) == ("changed", f"{page}#blur changed, as its entry in {help_}", None)
    # The page whole, and an anchor that is no card, compare everything the page is written from.
    assert check_atlas.state(page, None, "base", history, generated) == ("changed", f"{page} changed, as its entry in {help_}", None)
    assert check_atlas.state(f"{page}#blur-details", None, "base", history, generated)[0] == "changed"
    history = History(changed={help_}, texts={("base", help_): catalog("Blurs more.", intro="Modifiers bend a layer.")})
    assert check_atlas.state(f"{page}#modifiers", None, "base", history, generated)[0] == "changed"   # the intro is the catalog's entry
    history = History(changed={help_}, texts={("base", help_): "{ not json"})
    assert check_atlas.state(f"{page}#mirror", None, "base", history, generated)[0] == "changed"   # an unreadable help.json then is no proof of sameness


def test_a_catalog_page_with_a_written_intro_compares_its_cards_through_help_json_and_its_intro_on_disk(now):
    page, help_ = "docs/moonmodules/light/modifiers.md", "docs/help/help.json"
    now(page, "# Modifiers\n\nWritten intro, changed.\n")
    now(help_, catalog("Blurs."))
    history = History(changed={help_, page}, texts={("base", help_): catalog("Blurs."), ("base", page): "# Modifiers\n\nWritten intro.\n"})
    generated = frozenset({page})
    assert check_atlas.state(f"{page}#blur", None, "base", history, generated) == ("current", "", None)   # its card is the same; the intro on disk holds no card
    assert check_atlas.state(f"{page}#modifiers", None, "base", history, generated) == ("changed", f"{page}#modifiers changed", None)


def test_an_html_id_section_runs_to_the_next_heading_of_any_level():
    page = ("## A", "", '<a id="card"></a>', "card text", "", "#### Deeper", "", "after")
    assert check_atlas.section(page, "card") == ('<a id="card"></a>', "card text", "")
    assert check_atlas.section(page, "a") == page   # a level-2 section holds the level-4 one


def test_a_section_is_compared_alone_so_a_change_elsewhere_on_its_page_leaves_its_claim_current(now):
    now("docs/p.md", "Intro added.\n\n## A\n\none\n\n## B\n\nthree\n")
    history = was("## A\n\none\n\n## B\n\ntwo\n", "docs/p.md")
    assert check_atlas.state("docs/p.md#a", None, "base", history) == ("current", "", None)
    assert check_atlas.state("docs/p.md#b", None, "base", history) == ("changed", "docs/p.md#b changed", None)


def test_a_claim_is_named_by_its_worst_citation_and_one_being_written_is_left_alone(now, tmp_path):
    claims = "docs/c.json"
    now(claims, '{"claims": [\n {"id": "c-001"},\n {"id": "c-002"},\n {"id": "c-003"}\n]}\n')
    now("src/a.h", "new\nint firstLine = citedOnce();\nint secondLine = citedTwice();\nTHREE\n")
    g = gen_atlas.Graph()
    for cid, lines in (("c-001", "1-2"), ("c-002", "3"), ("c-003", "1")):
        g.edge(g.node(f"claim:{claims}#{cid}", "claim", cid, claims), "cites", "file:src/a.h", lines)
    gen_atlas.write(g, {"fingerprint": "x"}, tmp_path / "a.sqlite")
    history = was("int firstLine = citedOnce();\nint secondLine = citedTwice();\nthree\n")
    history._entries = {claims: [("c-001", "base"), ("c-002", "base"), ("c-003", None)]}
    with contextlib.closing(sqlite3.connect(tmp_path / "a.sqlite")) as c:
        found = check_atlas.stale(c, history)
    assert [(f["rule"], f["message"], f["line"]) for f in found] == [
        ("citation moved", f"{claims}#c-001: src/a.h:1-2 is now 2-3", 2),
        ("claim stale", f"{claims}#c-002: src/a.h:3 changed", 3)]
    assert all("level" not in f for f in found)   # warnings: the fact may still hold
    assert found[0]["fixes"] == [{"claim": "c-001", "from": "src/a.h:1-2", "to": "src/a.h:2-3"}]
    assert "fixes" not in found[1]


def test_only_a_claim_whose_other_citations_are_current_carries_fixes_since_a_rewrite_would_make_its_changed_text_the_baseline(now, tmp_path):
    claims = "docs/c.json"
    now(claims, '{"claims": [\n {"id": "c-001"},\n {"id": "c-002"}\n]}\n')
    now("src/a.h", "new\nint firstLine = citedOnce();\nint secondLine = citedTwice();\nTHREE\n")
    g = gen_atlas.Graph()
    for cid, cites in (("c-001", ("1-2", "3")), ("c-002", ("2-2",))):
        claim = g.node(f"claim:{claims}#{cid}", "claim", cid, claims)
        for lines in cites:
            g.edge(claim, "cites", "file:src/a.h", lines)
    gen_atlas.write(g, {"fingerprint": "x"}, tmp_path / "a.sqlite")
    history = was("int firstLine = citedOnce();\nint secondLine = citedTwice();\nthree\n")
    history._entries = {claims: [("c-001", "base"), ("c-002", "base")]}
    with contextlib.closing(sqlite3.connect(tmp_path / "a.sqlite")) as c:
        found = check_atlas.stale(c, history)
    assert [(f["rule"], f.get("fixes")) for f in found] == [
        ("claim stale", None),   # its first citation moved, its second changed: a person re-reads it
        ("citation moved", [{"claim": "c-002", "from": "src/a.h:2-2", "to": "src/a.h:3-3"}])]   # a range stays a range


def test_a_review_finding_a_claim_correct_verifies_it_from_the_review_commit_and_another_verdict_does_not(now, tmp_path):
    claims, review = "docs/work/present/atlas/areas/core-x.json", "docs/work/present/atlas/areas/review.json"
    now(review, json.dumps({"claims": [{"file": "core-x", "id": "core-001", "verdict": "correct"},
                                       {"file": "core-x", "id": "core-002", "verdict": "partly"}]}))
    g = gen_atlas.Graph()
    for cid in ("core-001", "core-002"):
        g.edge(f"file:{review}", "reviews", g.node(f"claim:{claims}#{cid}", "claim", cid, claims))
    gen_atlas.write(g, {"fingerprint": "x"}, tmp_path / "a.sqlite")
    history = History(entries={claims: [("core-001", "old"), ("core-002", "old")], review: [("core-001", "new"), ("core-002", "new")]},
                      order={"new": 0, "old": 1})
    with contextlib.closing(sqlite3.connect(tmp_path / "a.sqlite")) as c:
        assert check_atlas.baselines(c, history) == {f"claim:{claims}#core-001": "new", f"claim:{claims}#core-002": "old"}


def test_a_review_whose_entries_do_not_line_up_with_their_ids_counts_for_nothing(now, tmp_path):
    claims, review = "docs/work/present/atlas/areas/core-x.json", "docs/work/present/atlas/areas/review.json"
    now(review, json.dumps({"claims": [{"file": "core-x", "id": "core-001", "verdict": "correct"}]}))
    g = gen_atlas.Graph()
    g.edge(f"file:{review}", "reviews", g.node(f"claim:{claims}#core-001", "claim", "core-001", claims))
    gen_atlas.write(g, {"fingerprint": "x"}, tmp_path / "a.sqlite")
    history = History(entries={claims: [("core-001", "old")], review: [("core-007", "new")]}, order={"new": 0, "old": 1})
    with contextlib.closing(sqlite3.connect(tmp_path / "a.sqlite")) as c:
        assert check_atlas.baselines(c, history) == {f"claim:{claims}#core-001": "old"}


def test_an_error_exits_1_and_warnings_alone_exit_0(tmp_path, monkeypatch):
    for name, module, code in (("warned", None, 0), ("errored", "Bare", 1)):
        g = gen_atlas.Graph()
        g.edge("file:test/unit_x.cpp", "tests", "module:Nowhere")   # an edge to nothing, a warning
        if module:
            g.node(f"module:{module}", "module", module)   # no doc section, an error
        db = tmp_path / f"{name}.sqlite"
        gen_atlas.write(g, {"fingerprint": "x", "skipped": ""}, db)
        monkeypatch.setattr(gen_atlas, "DB", db)
        monkeypatch.setattr(gen_atlas, "refresh", lambda: "refreshed")
        assert check_atlas.main() == code


def test_git_gives_each_claim_the_commit_of_its_own_lines_so_fixing_one_claim_leaves_the_others_stale(tmp_path, monkeypatch):
    monkeypatch.setattr(check_atlas, "ROOT", tmp_path)
    check_atlas.text_of.cache_clear()

    def git(*args):
        return subprocess.run(["git", *args], cwd=tmp_path, check=True, capture_output=True, text=True).stdout.strip()

    def commit(files, message):
        for path, text in files.items():
            (tmp_path / path).parent.mkdir(parents=True, exist_ok=True)
            (tmp_path / path).write_text(text)
            check_atlas.text_of.cache_clear()
        git("add", "-A")
        git("commit", "-qm", message)
        return git("rev-parse", "HEAD")

    git("init", "-q")
    git("config", "user.email", "t@t")
    git("config", "user.name", "t")

    def claims(*entries, notes="read on a clean\u2028tree"):
        """A claims file as a researcher writes it, one key per line, its notes after the list, with a character `splitlines` reads as a line end."""
        return json.dumps({"claims": [{"id": i, "cites": [c]} for i, c in entries], "notes": notes}, indent=1, ensure_ascii=False) + "\n"

    def review(*files):
        """A review file, each entry naming its claims file on the line before its id."""
        return json.dumps({"claims": [{"file": f, "id": f"c-00{n}", "verdict": "correct"} for n, f in enumerate(files, 1)]}, indent=1) + "\n"

    flat = '{"claims": [\n {"file": "claims", "id": "c-001", "verdict": "correct"},\n {"id": "c-002", "file": "claims", "verdict": "correct"}\n]}\n'
    research = commit({"src/a.h": "one\n", "claims.json": claims(("c-001", "src/a.h:1"), ("c-002", "src/b.h:9")),
                       "review.json": review("claims", "claims"), "flat.json": flat}, "research")
    commit({"src/a.h": "ONE\n"}, "code changes what c-001 cites")
    fix = commit({"claims.json": claims(("c-001", "src/a.h:1"), ("c-002", "src/b.h:1"))}, "fix c-002")
    renamed = commit({"claims.json": claims(("c-001", "src/a.h:1"), ("c-002", "src/b.h:1"), notes="and a second pass"),
                      "review.json": review("claims", "moved")}, "the notes and the second review entry's file change")
    second = json.loads(claims(("c-001", "src/a.h:1"), ("c-002", "src/b.h:1"), notes="and a second pass"))
    commit({"claims.json": json.dumps(second, indent=3, ensure_ascii=False) + "\n"}, "reindent: whitespace only")
    reword = commit({"claims.json": claims(("c-001", "src/a.h:1"), ("c-002", "src/b.h:1-1"), notes="and a second pass")}, "a change listed as none")
    (tmp_path / ".git-blame-ignore-revs").write_text(f"{reword}\n")
    (tmp_path / "claims.json").write_text(claims(("c-001", "src/a.h:1"), ("c-002", "src/b.h:1-1"), ("c-003", "src/a.h:1"), notes="and a second pass"))   # appending puts a comma after c-002
    check_atlas.text_of.cache_clear()
    history = check_atlas.History()
    assert history.entries("claims.json") == [("c-001", research), ("c-002", fix), ("c-003", None)]   # the notes, a reindent and a listed commit are no verification
    assert history.entries("review.json") == [("c-001", research), ("c-002", renamed)]   # a `file` line is its own entry's
    assert history.entries("flat.json") == [("c-001", research), ("c-002", research)]   # an entry on one line, its id first or not
    assert history.refused == set()
    (tmp_path / ".git-blame-ignore-revs").write_text("deadbeef\n")   # git refuses an abbreviated id
    refused = check_atlas.History()
    assert refused.entries("review.json") == [("c-001", research), ("c-002", renamed)]   # read without the list, rather than not at all
    assert refused.refused == {"git blame refuses .git-blame-ignore-revs, so blame read without it"}
    assert history.changed(research) == {"src/a.h", "claims.json", "review.json"}
    assert history.text(research, "src/a.h") == ("one",) and history.text(research, "src/b.h") is None


def test_an_edge_with_an_end_no_source_defines_is_named_but_a_base_class_outside_the_tree_is_not(con):
    found = check_atlas.dangling(con)
    assert messages(found) == [
        f"reviews claim:{CLAIMS}#core-099, which no source defines",
        "tests module:Nowhere, which no source defines"]
    assert found[1]["file"] == "test/unit/unit_x.cpp"


def test_a_module_with_no_doc_section_is_named(con):
    assert messages(check_atlas.undocumented(con)) == ["Bare has no doc section in help.json"]


def test_a_source_gen_atlas_skipped_is_an_error_since_every_other_rule_then_reads_a_partial_graph(tmp_path):
    for name, skipped, expected in (("partial", "tests,claims", ["gen_atlas skipped its tests stage", "gen_atlas skipped its claims stage"]),
                                    ("whole", "", [])):
        db = tmp_path / f"{name}.sqlite"
        gen_atlas.write(gen_atlas.Graph(), {"fingerprint": "", "skipped": skipped}, db)
        with contextlib.closing(sqlite3.connect(db)) as c:
            assert messages(check_atlas.skipped(c)) == expected
