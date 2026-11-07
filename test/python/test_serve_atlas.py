"""MoonAtlas's MCP server answers an agent as the command line answers a person, through the MCP SDK's own in-process client: the tools it lists are the declarations, an answer is data under its schema with the text beside it, and every way a call can fail is an error result that says what would succeed.

The graph is test_show_atlas's, written for the questions, and its refresh is stood in.
"""

import asyncio
import json
import re
import sys
from pathlib import Path

import pytest

mcp = pytest.importorskip("mcp")

import _verbs  # noqa: E402
import check_atlas  # noqa: E402
import gen_atlas  # noqa: E402
import serve_atlas  # noqa: E402
import show_atlas  # noqa: E402
from test_show_atlas import con  # noqa: E402,F401  the test graph

ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture
def client(con, tmp_path, monkeypatch):
    """Ask the server over MCP: list its tools, or call each (name, arguments) given, in one session."""
    monkeypatch.setattr(gen_atlas, "refresh", lambda: "MoonAtlas: up to date")
    monkeypatch.setattr(gen_atlas, "DB", tmp_path / "atlas.sqlite")

    def ask(*calls):
        async def session():
            async with mcp.Client(serve_atlas.server) as c:
                if not calls:
                    return c.instructions, (await c.list_tools()).tools
                return [await c.call_tool(name, args) for name, args in calls]
        return asyncio.run(session())
    return ask


def test_the_tools_listed_are_the_declarations_less_the_3d_views_feed(client):
    _, tools = client()
    listed = [t.model_dump(by_alias=True, exclude_none=True) for t in tools]
    served = [v for v in show_atlas.TOOLS + check_atlas.TOOLS + serve_atlas.fix_atlas.TOOLS if v.get("serve", True)]
    assert listed == [_verbs.tool(v, "atlas_") for v in served]
    assert "atlas_cloud" not in {t.name for t in tools}


def test_an_answer_is_data_under_the_answer_schema_with_the_text_a_person_reads_beside_it(client, capsys):
    jsonschema = pytest.importorskip("jsonschema")
    schema = json.loads(show_atlas.SCHEMA.read_text(encoding="utf-8"))
    calls = [("atlas_what", {"name": "Layer"}), ("atlas_impact", {"name": "Layer.h", "only": ["docs"], "limit": 1}), ("atlas_gaps", {}), ("atlas_schema", {})]
    for (name, _), r in zip(calls, client(*calls)):
        assert not r.is_error, name
        jsonschema.validate(r.structured_content, schema)
        assert r.content[0].text.splitlines()[0] in "\n".join(show_atlas.text(r.structured_content)), name
    impact = client(("atlas_impact", {"name": "Layer.h", "only": ["docs"], "limit": 1}))[0]
    assert impact.structured_content["more"] == {"left": 1, "limit": 2} and "Left out 1 row(s)" in impact.content[0].text
    assert capsys.readouterr().out == ""   # stdout carries the protocol, so nothing a verb prints reaches it


def test_a_name_nothing_holds_an_unknown_tool_and_a_refused_argument_are_errors_naming_what_would_succeed(client):
    nowhere, tool, limit, kind = client(("atlas_what", {"name": "Layr"}), ("atlas_where", {}),
                                        ("atlas_what", {"name": "Layer", "limit": -1}), ("atlas_impact", {"name": "Layer.h", "kind": ["nope"]}))
    assert all(r.is_error for r in (nowhere, tool, limit, kind))
    assert "the names closest to it: Layer" in nowhere.content[0].text and "ask rather than try other spellings" in nowhere.content[0].text
    assert "its tools are atlas_what" in tool.content[0].text
    assert limit.content[0].text.startswith("atlas_what: limit: -1 is less than the minimum of 0")
    assert kind.content[0].text.startswith("atlas_impact: kind.0: 'nope' is not one of")


def test_a_fix_dry_run_reports_what_would_change_and_writes_nothing(client, tmp_path, monkeypatch):
    claims = tmp_path / "c.json"
    claims.write_text('{"claims": [\n {"id": "c-001", "cites": ["src/a.h:1-2"]}\n]}\n')
    monkeypatch.setattr(check_atlas, "ROOT", tmp_path)
    monkeypatch.setattr(check_atlas, "stale", lambda con: [{"rule": "citation moved", "file": "c.json", "message": "",
                                                            "fixes": [{"claim": "c-001", "from": "src/a.h:1-2", "to": "src/a.h:3-4"}]}])
    before = claims.read_text()
    r = client(("atlas_fix", {"dry_run": True}))[0]
    assert not r.is_error and r.structured_content["dry_run"] is True and r.structured_content["files"][0]["problem"] == ""
    assert claims.read_text() == before


def test_the_tool_list_and_the_instructions_stay_small_enough_to_load_whole(client):
    instructions, tools = client()
    listed = json.dumps([t.model_dump(by_alias=True, exclude_none=True) for t in tools])
    assert len(listed) / 3.5 < 2000   # tokens, by the rough rule for JSON; Claude Code turns to tool search past a tenth of the context
    assert instructions == serve_atlas.INSTRUCTIONS and len(instructions) / 4 < 300


def test_the_server_and_the_python_tests_pin_the_same_mcp():
    script = (ROOT / "moondeck/docs/serve_atlas.py").read_text(encoding="utf-8")
    sys.path.insert(0, str(ROOT / "moondeck" / "test"))
    import test_host
    assert re.search(r'"(mcp==[^"]+)"', script)[1] in test_host.PY_DEPS


def test_an_unexpected_failure_is_an_error_result_naming_it_rather_than_the_sdks_bare_internal_error(client, monkeypatch):
    monkeypatch.setattr(check_atlas, "findings", lambda con: (_ for _ in ()).throw(RuntimeError("git is not on PATH")))
    r = client(("atlas_check", {}))[0]
    assert r.is_error and r.content[0].text.startswith("atlas_check failed: RuntimeError('git is not on PATH')")


def test_a_whole_number_sent_as_a_float_is_taken_as_the_integer_it_is_and_a_bad_regex_is_refused_by_name(client):
    limit, regex = client(("atlas_impact", {"name": "Layer.h", "only": ["docs"], "limit": 1.0}), ("atlas_fix", {"dry_run": True, "match": "("}))
    assert not limit.is_error and len(limit.structured_content["groups"][0]["rows"]) == 1
    assert regex.is_error and regex.content[0].text.startswith("atlas_fix: --match is not a regular expression")
