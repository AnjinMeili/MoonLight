# Plan: MoonAtlas, a knowledge graph of the repository

Plan of 2026-10-29, a Feature, approved. A new maintainer or agent asks what a module is, why it is shaped that way, how it runs, and what a change to it touches. Today each one answers by reading: about 1,800 tracked files, 145 modules, a 1,400-line MoonDeck reference and 13 plans in flight. MoonAtlas answers those questions from one query, and refreshes at build, so discovery is done once.

Built on a branch in a fork, to the project's standards. Everything it produces is saved on that branch, and the maintainer advises on what goes upstream once the toolset is proven.

**The names.** The scripts take MoonDeck's verbs (`gen_`, `show_`, `check_`). Where the project has no word for the new thing, the name is a lunar feature: Atlas is a crater on the Moon, and an atlas is a book of maps.

## What exists

- **Modules:** `docs/help/help.json`, generated from the headers by `gen_help` through libclang, holds every module with its base, controls, doc page and source. `check_docgen` proves it fresh.
- **Module to spec:** `check_specs` pairs each module header with its `docs/moonmodules/` page.
- **Includes:** `check_module` resolves which translation units reach a module, following `#include` edges.
- **Tests:** `_test_metadata.parse_unit_file` reads the unit tests and what each pins.
- **Processes:** `moonwalk/*.yaml`, each step with the command or process it runs.
- **Reasons:** a `@xref{...}` in code points at a `## Why` section in a `///` appendix; the architecture pages and module pages hold the rest.
- **Rules:** `coding-standards.md` and `documentation-standards.md`, each rule enforced by a check whose MoonDeck card links its section, or left to review.
- **A generator in the build:** the desktop CMake runs `generate_build_info.py` through `uv` on every build.

The graph adds no new facts. It joins these, so every node points at the one home of what it says.

## Design

**Storage is SQLite**, from Python's standard library: a `nodes` table, an `edges` table and recursive queries, the textbook adjacency list. One file, no server, no dependency. It lives at `build/atlas/atlas.sqlite`, a generated artifact like the build itself, rebuilt from the tree in seconds, so the database file stays out of git while everything that costs tokens to make is committed.

**Nodes:** `file` (every file in the tree, a test, a script, a plan and a page the docs build generates included), `class`, `module`, `control`, `section` (a page's anchor, or a `/// ##` section in code), `process` and `step` (MoonWalk), `claim`.

**Edges:** `includes` (file to file), `imports` (Python file to file), `defines` (file to class), `derives` (class to base), `is` (module to class), `defined_in` (module to header), `documented_by` (module, process or script to its page or section), `has_control`, `in` (section to page, step to process), `xrefs` (code to its own section), `mentions` (page to a ticked class or module), `tests` and `proves` (test file to module and to MoonWalk step; a pytest file tests what it imports), `runs` (step to script or process), `enforces` (check to the standard it links), `cites` (claim to file or section), `reviews` (a blind review to each claim it checked, carrying the verdict). A rule is the standards section a check's `enforces` edge lands on.

**A change rebuilds the whole graph.** A run takes one stat pass over the tree (tracked files plus untracked ones git does not ignore), 0.03 s, and when anything changed rebuilds everything, 0.6 s on 1,800 files, written to a new file and renamed into place. A per-file update would save half a second for the cost of rows going stale.

**Discovery adds claims, never prose of its own.** A Researcher agent returns claims in a fixed JSON schema: what, why, how, an invariant or a gotcha, each citing a `file:line` or a `doc#anchor`. A claim whose citation does not resolve leaves its file before it is committed: a file outside the tree, a line past its end, an anchor its page lacks. Each citation is a `cites` edge to the file or section, and the claim's `home` field says whether that fact is already written in a doc, a comment or nowhere. A claim written nowhere is a **gap**: a sentence the docs are missing, with the page where it belongs. A blind review's verdict stays in its review file, joined onto the claim at build: a claim keeps the researcher's words and prints with the reviewer's finding before them, and a claim the review found already written is no gap. A claim was last verified in the newer of the commit its own lines last changed in and a review that found it correct, so git holds the baseline and a claim carries none of its own. Research reads a clean tree, so that commit holds the text the researcher read. Each citation is compared with its text in that commit, a section for an anchor and the lines for a range: unchanged is current, the same text once elsewhere has moved, and anything else makes the claim stale, a sentence a person re-reads.

**Standards stay where they are.** Rules become nodes pointing at their section in the two standards pages, and the query lists which check enforces each rule and which are left to review.

## Steps

1. ✅ **The mechanical layer.** `moondeck/docs/gen_atlas.py` builds and updates the graph from the sources above, reusing their parsers rather than copying them. A MoonDeck card, an entry in `moondeck_config.json`, and `test/python/test_gen_atlas.py` pinning each extractor firing on input built for it and staying silent on input without it.
2. ✅ **The query.** `moondeck/docs/show_atlas.py`: `what`, `why` (follows `documented_by`, and lists every `/// ##` section of a module's header and implementation), `impact` (everything reaching a file or module: includes, tests, docs, scenarios, steps), `tests`, `rules` and `gaps`, pinned by `test/python/test_show_atlas.py`. A query refreshes the graph first, so an answer describes the tree as it is.
3. 🚧 **The pilot, which measures cost.** Four Researcher agents, read-only and in parallel: MoonModule with core system (`src/core/module`, `src/core/system`) and the light pipeline (`src/light/layouts`, `layers`, `modifiers`, `util`), each area run once on Fable and once on Sonnet with the same prompt, so the two models compare on the same code. It runs ahead of steps 1 and 2, since the claims follow `atlas/claims.schema.json` rather than the tools. A script rejects each claim whose citation does not resolve, then a Reviewer agent checks a sample of the rest. Run: $20.90, results in [atlas/pilot/record.md](atlas/pilot/record.md), per agent: tokens, minutes, tool calls, claims, the share verified and gaps. Step 1 now exists, so the share the mechanical layer answers is the measurement still due. The claims and this record are committed under `docs/work/present/atlas/`, and the go or no-go for step 6 is read from them.
4. ✅ **Refresh at build.** `gen_atlas.py`, which rebuilds only when the tree changed, runs beside `generate_build_info.py` in the desktop build: 0.03 s on an unchanged tree, 0.07 s with uv's start. A failure prints and leaves the build green: `main()` catches any failure, the tree walk and the write included, prints it to stderr and exits 0, so the graph degrades visibly and never stops a build.
5. ✅ **The check.** `moondeck/check/check_atlas.py`: a citation that does not resolve (by the rules under Design), a stale claim, an edge whose node is gone, and a module with no `documented_by`, reported in the `_findings` format. A MoonDeck card, and `test/python/test_check_atlas.py` pinning each finding firing and staying silent. Whether it joins the commit gate is decided from step 3's numbers. First run: two citations one line past their file's end, corrected. Under the file-level rule, 153 claims were stale, nearly all citing `moondeck.py` or `MoonDeck.md`; compared citation by citation, 66 of them cite text that is unchanged, 78 cite text that only moved, and 9 cite text that changed. Seven edges point at nothing: six test `@module` names that are no module, class or file stem, and a review of `deck-016`, removed once its fix made it false.
6. 🚧 **The remaining areas**, each researcher given [the one prompt](atlas/researcher-prompt.md) on a clean tree, results in [atlas/areas/record.md](atlas/areas/record.md):
    - ✅ drivers and platform, $35.08 with review.
    - ✅ MoonDeck and CI, $32.13 with review.
    - ✅ services, util and the web UI, $24.80 with review.
    - ✅ effects; boot images, installer, cloud and release; MoonLive, $26.34 with review.
    - ⏳ tests, scenarios and MoonTube; processes and `docs/work`: candidates to skip, since the graph already joins them through `tests`, `proves` and `mentions`.
7. ⏳ **Which model reviews best.** A blind review decides which claims a reader trusts, so the model that reviews is chosen by how often it is right. The five reviews so far, all Fable, found 255 of 272 claims correct, 17 partly right and none wrong, so agreeing with them measures leniency rather than accuracy, and favors Fable. The test set therefore has a known answer for every claim:
    - **60 reviewed claims** across the areas, `correct` and `partly` mixed, which score agreement; where the models disagree, the product owner rules, and the ruling scores accuracy.
    - **40 seeded faults**, each a claim reviewed `correct` with one known change, ten of each kind: a citation moved to lines that do not show the fact (`cites_support` false), a fact reversed (`wrong`), a fact overstated (`partly`), a home named wrong (`home_ok` false). The reviewers are not told, and the order is shuffled.

    Three models through OpenRouter, at the same reasoning effort, high: `anthropic/claude-opus-5.5`, `anthropic/claude-fable-5.1` and `deepseek/deepseek-v4-pro-0813`, DeepSeek's top tier at its newest dated release. One harness runs all three, so the comparison measures the model rather than the agent around it: [the one reviewer prompt](atlas/reviewer-prompt.md), and a tool loop that reads and searches an export of the commit under test, without `docs/work/present/atlas/`, where the earlier verdicts would give the answers away. Each answer is validated against `atlas/review.schema.json`, whose verdict is `correct`, `partly` or `wrong`. The key is read from `~/.config/openrouter/api_key`, outside the repository. Measured per model: the seeded faults caught by kind, the false alarms on claims known correct, agreement on the reviewed claims, tokens, minutes and cost. The harness and the seeded claims live in `build/atlas-tools/`; the prompt, the schema and the results are committed, the results in `atlas/reviewers/record.md`, and the product owner picks the model the next review rounds run on.
8. ⏳ **The maintainer's advice.** The proven toolset, the pilot's numbers and a sample of `show_atlas` answers go to the maintainer, who advises what goes upstream: the tools alone, the claims, or only the gaps as doc changes. What goes is split into pull requests of at most 400 lines and ten files each.

## Open questions

- **The claims' home upstream**, decided at step 8 with the maintainer. On the branch they live under `docs/work/present/atlas/`.

## What it removes

Nothing in the tree during the pilot. It removes the discovery each session repeats. Once the graph answers what they hold, the hand-kept inventories under `docs/work/future/` (such as `processes-inventory.md`) are candidates to go, the product owner's call.
