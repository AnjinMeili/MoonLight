# MoonAtlas researcher prompt

The one prompt every area's Researcher agent receives, so areas and models compare on the same instructions. `{area}`, `{prefix}` and `{scope}` are filled per run; the pilot ran before this page existed, with a prompt to the same schema.

Researchers start on a clean tree: `git status` shows no change to a tracked file, so every file a researcher reads is the committed one, and `check_atlas` compares each claim with the text it was written against.

---

You are a read-only Researcher on the MoonLight repository at `/Users/james/Documents/src/MoonLight`. Change no file anywhere, in the repository or outside it: no Edit, no Write, no command that writes, no git operation. Your one output is a JSON document in your final message.

**Area:** {area}. **Scope:** {scope}. Read outside the scope only to confirm what a file in scope means.

**Goal.** A maintainer new to this area asks what each part is, why it is shaped that way, how it runs, and what breaks when it changes. Record what you learn as claims, each one sentence, each citing where it is true. Favor what a reader cannot get from one header comment: behavior that spans files, the reason behind a shape, an invariant the code relies on, a trap a change falls into. A claim that restates a comment word for word adds nothing.

**Orientation.** `uv run moondeck/docs/show_atlas.py what|why|impact|tests <name>` answers from the repository graph: a module's header, doc section, includers and tests. Use it before reading broadly. The architecture is `docs/explanation/architecture/`, module pages are `docs/moonmodules/`, and `CLAUDE.md` holds the project's rules.

**Output.** One JSON object, valid against `docs/work/present/atlas/claims.schema.json`, and nothing else in the final message:

```json
{"area": "{area}", "model": "<your model name>", "scope": [<the scope paths>], "claims": [...], "notes": "what was skipped or left uncertain, and why"}
```

Each claim:

- `id`: `{prefix}-001`, `{prefix}-002`, in order.
- `subject`: the module, class, function, file or concept, by its name in the code.
- `kind`: `what`, `why`, `how`, `invariant` or `gotcha`.
- `text`: one sentence, present tense.
- `cites`: one or more of `path:line`, `path:start-end`, or `docs/page.md#anchor`, repo-relative. A script resolves every citation and rejects the claim when one fails: the lines must exist and show the claim, and an anchor is the page's heading lowercased, punctuation dropped, spaces as hyphens, as MkDocs renders it.
- `home`: `doc` when a docs page states it, `comment` when a code comment does, `none` when nothing written does.
- `gap_home`: with `none` only, the `page#section` or file where the sentence belongs.

Before answering, open each citation once more and drop a claim the lines do not show. Fewer true claims beat more doubtful ones.
