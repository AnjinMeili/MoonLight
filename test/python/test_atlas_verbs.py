"""Each MoonAtlas verb is declared once, and its command line, its --schema and the MCP tool serve_atlas lists are read from that declaration, so these pin that the readings agree: the parser takes exactly the parameters the schema names, every example parses, every `next` names a verb, and a script prints data on stdout alone.
"""

import json
import shlex
import subprocess
import sys
from pathlib import Path

import pytest

import _verbs
import check_atlas
import fix_atlas
import show_atlas

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = [(show_atlas, "atlas_"), (check_atlas, "atlas_"), (fix_atlas, "atlas_")]
VERBS = [(m, v) for m, _ in SCRIPTS for v in m.TOOLS]


def subparser(module, verb):
    """The argparse parser a verb's options live on: the script's own for a script of one verb, else the verb's subcommand."""
    ap = _verbs.parser(module.SCRIPT, module.__doc__, module.TOOLS)
    if len(module.TOOLS) == 1:
        return ap
    sub = next(a for a in ap._actions if a.dest == "verb")
    return sub.choices[verb["name"]]


@pytest.mark.parametrize("module,verb", VERBS, ids=lambda x: getattr(x, "__name__", None) or x["name"])
def test_the_command_line_takes_exactly_the_parameters_the_input_schema_names_and_requires_the_same(module, verb):
    p = subparser(module, verb)
    options = {a.dest: a for a in p._actions if a.dest not in ("help", "json", "schema", "verb")}
    schema = _verbs.input_schema(verb)
    assert set(options) == set(schema["properties"])
    for name, a in options.items():
        assert (a.required or (not a.option_strings and a.nargs is None)) == (name in schema.get("required", [])), name


@pytest.mark.parametrize("module,verb", VERBS, ids=lambda x: getattr(x, "__name__", None) or x["name"])
def test_every_example_parses_and_every_next_names_a_declared_verb(module, verb):
    ap = _verbs.parser(module.SCRIPT, module.__doc__, module.TOOLS)
    for example in verb["examples"]:
        args = shlex.split(example) if len(module.TOOLS) == 1 else [verb["name"], *shlex.split(example)[1:]]
        ap.parse_args(args)
    declared = {v["name"] for m, _ in SCRIPTS for v in m.TOOLS}
    assert set(verb["next"]) <= declared
    assert verb["examples"] and all(len(module.TOOLS) == 1 or e.split()[0] == verb["name"] for e in verb["examples"])


def test_every_declaration_is_a_valid_mcp_tool_with_a_valid_input_schema_and_its_hints():
    jsonschema = pytest.importorskip("jsonschema")
    names = set()
    for module, prefix in SCRIPTS:
        for t in _verbs.schema(module.TOOLS, prefix):
            jsonschema.Draft202012Validator.check_schema(t["inputSchema"])
            assert set(t["annotations"]) == {"readOnlyHint", "destructiveHint", "idempotentHint", "openWorldHint"}
            assert t["annotations"]["readOnlyHint"] != t["annotations"]["destructiveHint"]
            assert t["reads"] and t["writes"] and set(t["exits"]) >= {0, 2}
            names.add(t["name"])
    assert len(names) == len(VERBS)   # no two verbs share a tool name


def test_only_fix_writes_to_the_tree_and_it_says_so_in_its_hints():
    writers = [v["name"] for _, v in VERBS if not v["annotations"]["readOnlyHint"]]
    assert writers == ["fix"] and fix_atlas.TOOLS[0]["annotations"]["destructiveHint"]


@pytest.mark.parametrize("argv", [["--schema"], []], ids=["schema", "bare"])
def test_the_schema_and_the_bare_listing_print_on_stdout_alone_read_nothing_from_stdin_and_need_no_graph(argv):
    done = subprocess.run([sys.executable, str(ROOT / "moondeck/docs/show_atlas.py"), *argv], stdin=subprocess.DEVNULL,
                          capture_output=True, text=True, timeout=60)
    assert done.returncode == 0 and done.stderr == ""
    if argv:
        assert {t["name"] for t in json.loads(done.stdout)} == {f"atlas_{v['name']}" for v in show_atlas.TOOLS}
    else:
        assert all(f"{show_atlas.SCRIPT} {v['examples'][0]}" in done.stdout for v in show_atlas.TOOLS)


def test_a_usage_error_exits_2_with_its_reason_on_stderr_and_nothing_on_stdout(capsys):
    for argv in (["impact", "Layer.h", "--only", "nowhere"], ["what"], ["rules", "--limit", "-1"], ["nope"]):
        with pytest.raises(SystemExit) as e:
            show_atlas.main(argv)
        out = capsys.readouterr()
        assert e.value.code == 2 and out.out == "" and "error:" in out.err


def test_the_schema_and_the_bare_listing_answer_with_no_graph_to_read(tmp_path, monkeypatch, capsys):
    import gen_atlas
    monkeypatch.setattr(gen_atlas, "DB", tmp_path / "never-built.sqlite")
    monkeypatch.setattr(gen_atlas, "refresh", lambda: (_ for _ in ()).throw(AssertionError("a declaration needs no refresh")))
    for argv in (["--schema"], []):
        assert show_atlas.main(argv) == 0 and capsys.readouterr().out
