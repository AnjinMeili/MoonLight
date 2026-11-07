"""MoonDeck's atlas routes hand a request to show_atlas, run as its own script, and its answer back: the 3D view's cloud as data, any other verb as the text a person reads, and a refusal as the HTTP status that names it.

show_atlas runs through `subprocess.run`, stood in here, so each test names the command MoonDeck builds and the status it gives each way the script can end.
"""

import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "moondeck"))

import moondeck  # noqa: E402


SCHEMA = json.dumps([{"name": f"atlas_{v}"} for v in ("what", "why", "impact", "tests", "rules", "gaps", "schema", "cloud")])


def ran(monkeypatch, code=0, out="", err=""):
    """Stand in for show_atlas: its declarations for `--schema`, and for any other command record it and end as asked."""
    calls = []
    moondeck._atlas_verbs.clear()

    def run(cmd, **kw):
        if cmd[3:] == ["--schema"]:
            return subprocess.CompletedProcess(cmd, 0, SCHEMA, "")
        calls.append(cmd[3:])   # after `uv run <script>`
        return subprocess.CompletedProcess(cmd, code, out, err)
    monkeypatch.setattr(moondeck.subprocess, "run", run)
    return calls


def test_the_graph_route_runs_the_cloud_with_its_options_and_passes_its_data_through(monkeypatch):
    calls = ran(monkeypatch, out=json.dumps({"verb": "cloud", "nodes": [], "links": []}))
    assert moondeck.atlas_api("/api/atlas/graph") == ({"verb": "cloud", "nodes": [], "links": []}, 200)
    moondeck.atlas_api("/api/atlas/graph?around=Layer.h&depth=2")
    assert calls == [["cloud"], ["cloud", "--around", "Layer.h", "--depth", "2"]]


def test_a_verb_route_returns_the_text_a_person_reads_with_the_refresh_line(monkeypatch):
    calls = ran(monkeypatch, out="tests: none\n", err="MoonAtlas: up to date\n")
    assert moondeck.atlas_api("/api/atlas/tests?name=src%2Fmain.cpp") == (
        {"verb": "tests", "query": "src/main.cpp", "text": ["tests: none"], "note": "MoonAtlas: up to date"}, 200)
    assert calls == [["tests", "src/main.cpp"]]
    moondeck.atlas_api("/api/atlas/what?name=Layer&limit=500")
    assert calls[-1] == ["what", "Layer", "--limit", "500"]   # the 3D view's panel asks for a module's edges whole


def test_a_verb_show_atlas_does_not_declare_is_a_400_without_starting_it(monkeypatch):
    calls = ran(monkeypatch)
    for path in ("/api/atlas/favicon.ico", "/api/atlas/cloud", "/api/atlas/"):
        body, status = moondeck.atlas_api(path)
        assert status == 400 and "it answers gaps, impact, rules, schema, tests, what, why and graph" in body["error"]
    assert calls == []


def test_a_refusal_is_400_a_name_nothing_holds_404_and_anything_else_500(monkeypatch):
    ran(monkeypatch, code=2, err="usage: show_atlas.py ...\nshow_atlas.py: error: argument --limit: --limit takes a whole number from 0\n")
    assert moondeck.atlas_api("/api/atlas/what?name=Layer") == ({"error": "show_atlas.py: error: argument --limit: --limit takes a whole number from 0"}, 400)
    ran(monkeypatch, code=1, err="MoonAtlas: up to date\nMoonAtlas: nothing is named Nowhere; a name is a module...\n")
    assert moondeck.atlas_api("/api/atlas/what?name=Nowhere")[1] == 404
    ran(monkeypatch, code=1, err="MoonAtlas: no graph to answer from; run gen_atlas to see why it cannot build one\n")
    assert moondeck.atlas_api("/api/atlas/rules") == ({"error": "MoonAtlas: no graph to answer from; run gen_atlas to see why it cannot build one"}, 500)


def test_a_script_that_cannot_start_or_does_not_finish_is_a_500_that_says_so(monkeypatch):
    moondeck._atlas_verbs.clear()

    def run(cmd, **kw):
        raise subprocess.TimeoutExpired(cmd, 120)
    monkeypatch.setattr(moondeck.subprocess, "run", run)
    body, status = moondeck.atlas_api("/api/atlas/graph")
    assert status == 500 and body["error"].startswith("show_atlas did not answer")
