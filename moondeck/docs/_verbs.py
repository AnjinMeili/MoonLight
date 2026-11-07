"""One declaration per verb: a script's command line, its --help, its --schema and the MCP tools serve_atlas lists are all read from it, so the paths cannot disagree.

A verb is a dict:

    name         the word on the command line; the MCP tool is the script's prefix and this name
    run          run(**params) -> the answer as data; raises LookupError when the question names nothing, Refused on an argument it refuses
    text         text(answer) -> the lines a person reads, or None for a verb that answers in data alone
    description  what it answers, the first line its summary
    params       {name: a JSON Schema property, plus `positional` and `required`}; a string, an integer, a boolean, or an array of enum strings, typed comma-separated on the command line
    annotations  MCP's hints: readOnlyHint, destructiveHint, idempotentHint, openWorldHint
    reads        what it reads; writes, what it writes, the tree and build/ alike
    examples     command lines after the script's name, each one that runs
    exits        {code: meaning}
    next         the verbs that follow it, by name
    code         code(answer) -> the exit code of an answer, 0 when left out
    serve        False for a verb the MCP server leaves out

Standard library alone, so MoonDeck and every script read it alike.
"""

import argparse
import json
import os
import sys


class Refused(Exception):
    """An argument a verb refuses, such as a regular expression that does not compile: a usage error, exit 2."""


def input_schema(verb: dict) -> dict:
    """The verb's parameters as MCP's inputSchema, a JSON Schema object."""
    props = {k: {kk: vv for kk, vv in p.items() if kk not in ("positional", "required")} for k, p in verb["params"].items()}
    required = [k for k, p in verb["params"].items() if p.get("required")]
    return {"type": "object", "properties": props, **({"required": required} if required else {}), "additionalProperties": False}


def tool(verb: dict, prefix: str) -> dict:
    """The verb as an MCP tool: its name, description, input schema and hints, the description closing on the tools that follow it."""
    follow = f" Next: {', '.join(prefix + n for n in verb['next'])}." if verb.get("next") else ""
    return {"name": prefix + verb["name"], "description": verb["description"] + follow, "inputSchema": input_schema(verb), "annotations": verb["annotations"]}


def schema(verbs: list[dict], prefix: str, output: dict | None = None) -> list[dict]:
    """Every verb's whole declaration for --schema: the MCP tool, the answer's schema, and what it reads, writes, exits with and leads to."""
    return [{**tool(v, prefix), **({"outputSchema": output} if output and v.get("text") else {}),
             "reads": v["reads"], "writes": v["writes"], "examples": v["examples"], "exits": v["exits"], "next": [prefix + n for n in v.get("next", [])]}
            for v in verbs]


def _comma(items: list[str], name: str):
    def parse(text: str) -> list[str]:
        values = [t.strip() for t in text.split(",") if t.strip()]
        bad = [t for t in values if t not in items]
        if bad or not values:
            raise argparse.ArgumentTypeError(f"--{name} takes a comma-separated list of {', '.join(items)}")
        return values
    return parse


def _bounded(name: str, low, high):
    def parse(text: str) -> int:
        try:
            n = int(text)
        except ValueError:
            n = None
        if n is None or (low is not None and n < low) or (high is not None and n > high):
            raise argparse.ArgumentTypeError(f"--{name} takes a whole number" + (f" from {low}" if low is not None else "") + (f" to {high}" if high is not None else ""))
        return n
    return parse


def _add(p: argparse.ArgumentParser, name: str, spec: dict):
    flag, help_ = f"--{name.replace('_', '-')}", spec.get("description", "")
    if "default" in spec and spec.get("type") != "boolean":
        help_ += f" (default: {spec['default']})"
    if spec.get("positional"):
        p.add_argument(name, nargs=None if spec.get("required") else "?", help=help_)
    elif spec["type"] == "boolean":
        p.add_argument(flag, dest=name, action="store_true", help=help_)
    elif spec["type"] == "integer":
        p.add_argument(flag, dest=name, type=_bounded(name, spec.get("minimum"), spec.get("maximum")), default=spec.get("default"), metavar="N", help=help_)
    elif spec["type"] == "array":
        p.add_argument(flag, dest=name, type=_comma(spec["items"]["enum"], name), default=spec.get("default"), metavar="A,B", help=help_)
    else:
        p.add_argument(flag, dest=name, choices=spec.get("enum"), default=spec.get("default"), help=help_)


def _epilog(script: str, verb: dict) -> str:
    return ("examples:\n" + "\n".join(f"  {script} {e}".rstrip() for e in verb["examples"])
            + "\n\nexit codes:\n" + "\n".join(f"  {c}  {m}" for c, m in verb["exits"].items()))


def parser(script: str, doc: str, verbs: list[dict]) -> argparse.ArgumentParser:
    """The command line: one verb a script with one, else a subcommand per verb, each with --json and its examples and exit codes in --help."""
    raw = argparse.RawDescriptionHelpFormatter
    single = len(verbs) == 1
    ap = argparse.ArgumentParser(prog=script, description=doc.strip(), formatter_class=raw, epilog=_epilog(script, verbs[0]) if single else None)
    ap.add_argument("--schema", action="store_true", help="print each verb's declaration as JSON: its inputs, hints, reads and writes, examples, exit codes and the verbs after it")
    targets = [(ap, verbs[0])] if single else []
    if not single:
        sub = ap.add_subparsers(dest="verb", metavar="verb")
        targets = [(sub.add_parser(v["name"], help=v["description"].splitlines()[0], description=v["description"], formatter_class=raw,
                                   epilog=_epilog(script, v)), v) for v in verbs]
    for p, v in targets:
        if v.get("text"):
            p.add_argument("--json", action="store_true", help="print the answer as data instead of text")
        for name, spec in v["params"].items():
            _add(p, name, spec)
    return ap


def listing(script: str, verbs: list[dict]) -> list[str]:
    """What a bare invocation prints: each verb with what it answers and one example."""
    width = max(len(v["name"]) for v in verbs)
    return ([f"{script}: ask the repository's knowledge graph"]
            + [f"  {v['name']:<{width}}  {v['description'].splitlines()[0]}\n  {'':<{width}}  e.g. {script} {v['examples'][0]}" for v in verbs]
            + [f"More: {script} <verb> --help; every declaration as JSON: {script} --schema"])


def main(script: str, doc: str, verbs: list[dict], prefix: str, output: dict | None = None, argv: list[str] | None = None) -> int:
    """Run the verb the command line names: its answer on stdout as text or JSON, everything else on stderr, 2 for a usage error, 1 for a question that names nothing."""
    ap = parser(script, doc, verbs)
    ns = ap.parse_args(argv)
    if ns.schema:
        print(json.dumps(schema(verbs, prefix, output), indent=1))
        return 0
    verb = verbs[0] if len(verbs) == 1 else next((v for v in verbs if v["name"] == ns.verb), None)
    if verb is None:
        print("\n".join(listing(script, verbs)))
        return 0
    try:
        answer = verb["run"](**{k: getattr(ns, k) for k in verb["params"]})
    except Refused as e:
        ap.error(str(e))
    except LookupError as e:
        print(e.args[0] if e.args else e, file=sys.stderr)
        return 1
    print(json.dumps(answer, indent=1) if getattr(ns, "json", False) or not verb.get("text") else "\n".join(verb["text"](answer)))
    return verb.get("code", lambda a: 0)(answer)


def script(*args, **kwargs):
    """Run main and exit with its code; a pipe closed early (`| head`) is a normal end, as Python's note on SIGPIPE has it."""
    try:
        code = main(*args, **kwargs)
        sys.stdout.flush()
    except BrokenPipeError:
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        code = 0
    sys.exit(code)
