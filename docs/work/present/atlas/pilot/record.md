# MoonAtlas pilot record

Step 3 of the [MoonAtlas plan](../../Plan-20261029%20-%20A%20knowledge%20graph%20of%20the%20repository.md), run on 2026-10-29. Two areas, each researched by Fable 5.1 and by Sonnet 5.5 with the same prompt, then a blind review by Fable 5.1.

- **core:** `src/core/module`, `src/core/system` (39 files, 0.8 MB)
- **pipeline:** `src/light/layouts`, `layers`, `modifiers`, `util` (44 files, 0.25 MB)

## Cost

Priced from each agent's API usage at list rates per million tokens: Fable $10 in, $50 out, $0.25 cache read; Sonnet $2 in, $10 out, $0.20 cache read; a cache write at 1.25 times input. The orchestrating session is not counted.

| Run | Cost | Minutes | Tool calls | Claims | Cost per claim |
|---|---|---|---|---|---|
| core, Fable | $7.06 | 8.9 | 67 | 80 | $0.088 |
| core, Sonnet | $1.26 | 4.0 | 37 | 113 | $0.011 |
| pipeline, Fable | $6.08 | 8.9 | 43 | 103 | $0.059 |
| pipeline, Sonnet | $0.87 | 2.5 | 18 | 114 | $0.008 |
| Review, Fable | $5.63 | 7.8 | 31 | 57 checked | $0.099 |
| **Total** | **$20.90** | | | 410 | |

Output tokens drive the cost: Fable wrote about 45,000 per run against Sonnet's 22,000, at five times the price.

## Accuracy

A script resolved all 832 citations first: one failed, a GitHub-style anchor (`core-sonnet` `core-113`), and that claim is removed from its file. The reviewer then checked every gap claim and 12 random claims per file, blind to the model.

| | Sampled | Correct | Partly | Wrong | Citation not showing it | Home wrong |
|---|---|---|---|---|---|---|
| Fable | 30 | 30 | 0 | 0 | 0 | 2 |
| Sonnet | 27 | 25 | 2 | 0 | 2 | 0 |

`core-sonnet` `core-006` reported that `moonmodule.md` names `onBuildState()` and `teardown()`, hooks renamed in `fa3066ac`. The page was fixed during the pilot, before the review ran, so the reviewer marked the claim wrong; it is counted correct here, as the pilot's one confirmed doc defect.

## Coverage

- **core: Sonnet covered more.** Its extra claims are the base-class mechanics (release order, child-array mutation, name and document limits); Fable's extras restate facts the service headers already state. Fable read every file in scope; Sonnet read the bodies of four.
- **pipeline: Fable covered more.** The reviewer credits it with three behaviors the comments and docs omit or contradict: the additive blend path is dead outside tests (`MappingLUT::overwrites` is never set false in production, confirmed in the sample), live modifiers leave the child-order chain, and the rebuild frame skips the live pass (both unchecked). Sonnet's pipeline claims mostly restate header comments.

## Findings for the product owner

- `docs/explanation/architecture/moonmodule.md` named the old lifecycle hooks: fixed on the `docs-match-code` branch, pull request #163.
- BlendMap's additive fold path runs only in tests: dead code or a missing feature, undecided.
- 9 gap claims across the four files name a fact written nowhere, each with the page where it belongs.

## A claim enters the docs once a reviewer opens the code

`pipe-fable` `pipe-040`, a rebuild frame "shows the static result", was cited and confident, and the review had listed it as unchecked. It reached a pull request before a second review read `Layer::prepare()`: the rebuild clears the buffer, so that frame shows black. A claim enters the docs only after a reviewer has opened the code it describes and followed the data to where it ends up.

## What it says about step 6

The remaining eight areas hold about 17.8 MB of tracked text, 17 times the pilot, much of it in `src/platform` and `test`, which split into smaller areas first. Cost grows with the reading an agent chooses, not with bytes, so the projection is a range:

- **Sonnet researchers:** about $30 to $60.
- **Fable researchers:** about $150 to $250.
- **Review**, Fable on a sample per area: about $25 to $45 on top of either.

Neither model won outright: Sonnet is seven to eight times cheaper per claim and slightly less precise; Fable finds the behaviors nobody wrote down. The share of claims the mechanical layer answers is the measurement still due, now that step 1 exists.
