"""fix_atlas points a moved citation at the lines it moved to, inside its own claim alone, each claims file whole or not at all; a claim whose cited text changed stays a person's to re-read.

The last test runs the round trip through git, as the tool is used: a cited block moves, check_atlas names the move, fix_atlas writes it, and after the commit check_atlas finds the claim current, while a stale claim beside it is still named.
"""

import contextlib
import json
import re
import sqlite3
import subprocess

import pytest

import check_atlas
import fix_atlas
import gen_atlas

CLAIMS = """{"claims": [
 {
  "id": "c-001",
  "text": "Two lines.",
  "cites": [
   "src/a.h:1-2",
   "src/a.h:12"
  ]
 },
 {"id": "c-002", "cites": ["src/a.h:1-2"]}
],
 "notes": "src/a.h:1-2 is mentioned here too"
}
"""


def fix(claim, frm, to):
    return {"claim": claim, "from": frm, "to": to}


def test_a_fix_rewrites_its_cite_inside_its_own_entry_and_leaves_every_other_line_as_it_was():
    after, why = fix_atlas.rewrite(CLAIMS, [fix("c-001", "src/a.h:1-2", "src/a.h:4-5")])
    assert why == ""
    before_lines, after_lines = CLAIMS.split("\n"), after.split("\n")
    assert [i for i, (b, a) in enumerate(zip(before_lines, after_lines)) if b != a] == [5]   # c-002's same cite and the notes stay
    assert after_lines[5] == '   "src/a.h:4-5",'
    assert after.endswith("}\n")
    after, _ = fix_atlas.rewrite(CLAIMS, [fix("c-001", "src/a.h:12", "src/a.h:14")])
    assert '"src/a.h:14"' in after and '"src/a.h:1-2",' in after   # `a.h:1` is never matched inside `a.h:12`, nor the other way
    after, _ = fix_atlas.rewrite(CLAIMS, [fix("c-002", "src/a.h:1-2", "src/a.h:3-4")])
    assert after.split("\n")[9] == ' {"id": "c-002", "cites": ["src/a.h:3-4"]}'   # an entry on one line


def test_two_cites_of_one_claim_that_move_onto_and_past_each_other_each_find_their_own():
    claim = '{"claims": [\n {"id": "c-001", "cites": ["a.h:1-2", "a.h:3-4"]}\n]}\n'
    chained = [fix("c-001", "a.h:1-2", "a.h:3-4"), fix("c-001", "a.h:3-4", "a.h:5-6")]   # two lines inserted above both
    assert fix_atlas.rewrite(claim, chained) == ('{"claims": [\n {"id": "c-001", "cites": ["a.h:3-4", "a.h:5-6"]}\n]}\n', "")
    swapped = [fix("c-001", "a.h:1-2", "a.h:3-4"), fix("c-001", "a.h:3-4", "a.h:1-2")]   # the two blocks traded places
    assert fix_atlas.rewrite(claim, swapped)[0] == '{"claims": [\n {"id": "c-001", "cites": ["a.h:3-4", "a.h:1-2"]}\n]}\n'


@pytest.mark.parametrize("bad, why", [
    (fix("c-009", "src/a.h:1-2", "src/a.h:3-4"), "c-009 is not in the file"),
    (fix("c-001", "src/b.h:1", "src/b.h:2"), "c-001 cites src/b.h:1 no times in its entry, not once"),
])
def test_a_fix_that_does_not_apply_exactly_once_leaves_the_text_unwritten_and_says_why(bad, why):
    assert fix_atlas.rewrite(CLAIMS, [fix("c-001", "src/a.h:12", "src/a.h:14"), bad]) == (None, why)
    twice = CLAIMS.replace('{"id": "c-002"', '{"id": "c-001"')
    assert fix_atlas.rewrite(twice, [fix("c-001", "src/a.h:12", "src/a.h:14")]) == (None, "c-001 is more than once in the file")


def moved(file, *fixes):
    return {"rule": "citation moved", "message": "m", "file": file, "fixes": list(fixes)}


@pytest.fixture
def tree(tmp_path, monkeypatch):
    monkeypatch.setattr(check_atlas, "ROOT", tmp_path)
    (tmp_path / "good.json").write_text(CLAIMS)
    (tmp_path / "bad.json").write_text(CLAIMS)
    return tmp_path


def test_a_dry_run_changes_nothing_and_a_file_with_one_bad_fix_is_left_whole(tree):
    findings = [moved("good.json", fix("c-001", "src/a.h:1-2", "src/a.h:4-5")),
                moved("bad.json", fix("c-001", "src/a.h:1-2", "src/a.h:4-5"), fix("c-009", "x:1", "x:2")),
                {"rule": "claim stale", "message": "m", "file": "good.json"}]
    report = fix_atlas.run(findings, None, dry_run=True)
    assert [(r["file"], r["problem"]) for r in report] == [("bad.json", "c-009 is not in the file"), ("good.json", "")]
    assert (tree / "good.json").read_text() == CLAIMS
    fix_atlas.run(findings, None, dry_run=False)
    assert '"src/a.h:4-5"' in (tree / "good.json").read_text()
    assert (tree / "bad.json").read_text() == CLAIMS   # its good fix is not written either
    assert fix_atlas.run(findings, re.compile("nomatch"), dry_run=False) == []


def test_a_file_changed_between_the_plan_and_the_write_is_left_as_it_now_is(tree):
    (tree / "good.json").write_text(CLAIMS + " ")
    assert fix_atlas.write(tree / "good.json", CLAIMS, "new") == "changed while the fixes were planned"
    assert (tree / "good.json").read_text() == CLAIMS + " "


def test_a_written_file_keeps_its_mode_and_no_temporary_file_is_left_beside_it_even_when_interrupted(tree, monkeypatch):
    (tree / "good.json").chmod(0o644)
    assert fix_atlas.write(tree / "good.json", CLAIMS, "new\n") == ""
    assert (tree / "good.json").stat().st_mode & 0o777 == 0o644
    assert sorted(p.name for p in tree.iterdir()) == ["bad.json", "good.json"]

    def interrupt(*_):
        raise KeyboardInterrupt
    monkeypatch.setattr(fix_atlas.os, "replace", interrupt)
    with pytest.raises(KeyboardInterrupt):
        fix_atlas.write(tree / "bad.json", CLAIMS, "new\n")
    assert sorted(p.name for p in tree.iterdir()) == ["bad.json", "good.json"]
    assert (tree / "bad.json").read_text() == CLAIMS


@pytest.fixture
def cli(tree, monkeypatch):
    """main() over the findings given, with the graph's refresh and database stood in."""
    def call(findings, *argv):
        monkeypatch.setattr(gen_atlas, "refresh", lambda: "refreshed")
        db = tree / "a.sqlite"
        gen_atlas.write(gen_atlas.Graph(), {"fingerprint": "x"}, db)
        monkeypatch.setattr(gen_atlas, "DB", db)
        monkeypatch.setattr(check_atlas, "stale", lambda con: findings)
        return fix_atlas.main(list(argv))
    return call


def test_the_command_line_reports_bounded_exits_1_on_a_fix_that_does_not_apply_and_2_on_a_usage_error(cli, capsys, tree):
    many = [moved("good.json", *[fix("c-001", "src/a.h:12", "src/a.h:14")])]
    assert cli(many, "--dry-run", "--limit", "0") == 0
    out = capsys.readouterr().out
    assert out.startswith("fix_atlas: would rewrite 1 citation(s) in 1 claim(s) across 1 file(s)\n")
    assert "showing 0 of 1; more: --limit 1" in out and "without --dry-run" in out
    assert cli([moved("bad.json", fix("c-009", "x:1", "x:2"))], "--json") == 1
    assert json.loads(capsys.readouterr().out)["files"][0]["problem"] == "c-009 is not in the file"
    assert cli([], "--json") == 0
    for argv in (["--match", "("], ["--limit", "-1"]):
        with pytest.raises(SystemExit) as e:
            cli([], *argv)
        assert e.value.code == 2


def test_through_git_a_fixed_and_committed_claim_reads_current_and_a_stale_claim_beside_it_stays_named(tmp_path, monkeypatch):
    monkeypatch.setattr(check_atlas, "ROOT", tmp_path)

    def git(*args):
        return subprocess.run(["git", *args], cwd=tmp_path, check=True, capture_output=True, text=True).stdout.strip()

    def commit(files, message):
        for path, text in files.items():
            (tmp_path / path).write_text(text)
        check_atlas.text_of.cache_clear()
        git("add", "-A")
        git("commit", "-qm", message)

    def findings():
        check_atlas.text_of.cache_clear()
        g = gen_atlas.Graph()
        for cid, lines in (("c-001", "1-2"), ("c-002", "3")):
            g.edge(g.node(f"claim:claims.json#{cid}", "claim", cid, "claims.json"), "cites", "file:a.h", lines)
        gen_atlas.write(g, {"fingerprint": "x"}, tmp_path / "a.sqlite")
        with contextlib.closing(sqlite3.connect(tmp_path / "a.sqlite")) as con:
            found = check_atlas.stale(con, check_atlas.History())
        return [(f["rule"], f["message"]) for f in found], found

    git("init", "-q")
    git("config", "user.email", "t@t")
    git("config", "user.name", "t")
    claims = json.dumps({"claims": [{"id": "c-001", "cites": ["a.h:1-2"]}, {"id": "c-002", "cites": ["a.h:3"]}]}, indent=1) + "\n"
    commit({"a.h": "int firstLine = citedOnce();\nint secondLine = citedTwice();\nthree\n", "claims.json": claims}, "research")
    commit({"a.h": "added\nint firstLine = citedOnce();\nint secondLine = citedTwice();\nTHREE\n"}, "code moves c-001's lines and changes c-002's")
    named, found = findings()
    assert named == [("citation moved", "claims.json#c-001: a.h:1-2 is now 2-3"), ("claim stale", "claims.json#c-002: a.h:3 changed")]
    assert [r["problem"] for r in fix_atlas.run(found, None, dry_run=False)] == [""]
    assert findings()[0] == [("claim stale", "claims.json#c-002: a.h:3 changed")]   # c-001 is being written until committed
    commit({}, "point c-001 at its new lines")
    assert findings()[0] == [("claim stale", "claims.json#c-002: a.h:3 changed")]   # c-001 is current; c-002 is still a person's
    assert json.loads((tmp_path / "claims.json").read_text())["claims"][1]["cites"] == ["a.h:3"]
