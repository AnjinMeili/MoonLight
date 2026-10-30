# MoonAtlas: the remaining areas

Step 6 of the [MoonAtlas plan](../../Plan-20261029%20-%20A%20knowledge%20graph%20of%20the%20repository.md), run on 2026-10-29, largest area of code first. Each area: Fable 5.1 researchers in parallel, each given [the researcher prompt](../researcher-prompt.md), a script resolving every citation against the tree as git sees it, then a blind review by Fable 5.1 of every gap claim and 12 random claims per file, following each path through to where its data ends.

Priced as in the [pilot](../pilot/record.md#cost), from each transcript's usage deduplicated by request: Fable $10 in, $50 out, $0.25 cache read, a cache write at 1.25 times input. The orchestrating session is not counted.

## Drivers and platform

- **drivers:** `src/light/drivers/` (27 files, 0.50 MB)
- **esp32:** `src/platform/esp32/`, `esp32/` (63 files, 0.76 MB)
- **platform:** the files directly in `src/platform/`, `src/platform/desktop/` without its vendored `miniaudio.h`, `src/platform/teensy/`, `teensy/` (39 files, 0.50 MB)

| Run | Cost | Minutes | Tool calls | Claims | Cost per claim |
|---|---|---|---|---|---|
| drivers | $9.79 | 15.5 | 47 | 147 | $0.067 |
| esp32 | $7.04 | 10.8 | 42 | 84 | $0.084 |
| platform | $14.20 | 15.4 | 55 | 139 | $0.102 |
| Review | $4.05 | 6.7 | 22 | 52 checked | $0.078 |
| **Total** | **$35.08** | | | 370 | |

| | Sampled | Correct | Partly | Wrong | Citation not showing it | Home wrong |
|---|---|---|---|---|---|---|
| drivers | 15 | 15 | 0 | 0 | 0 | 1 |
| esp32 | 22 | 22 | 0 | 0 | 0 | 1 |
| platform | 15 | 14 | 1 | 0 | 0 | 1 |

One citation fails: `esp-080` cites an ignored `esp32/.vscode/settings.json` and is removed from its file. `plat-053` overstates one half: a second refused enqueue also grows the failure streak, where the claim says only `ethFlushRaw` does. The three wrong homes are gap claims whose fact a comment or a page already states (`drv-145`, `esp-022`, `plat-009`), so 13 of the 16 gaps stand.

## MoonDeck and CI

- **checks:** `moondeck/check/` (23 files, 0.43 MB)
- **moondeck:** the server and its configuration, `moondeck_ui/`, `run/`, `scenario/`, `test/`, `diag/`, `qemu/`, `report/` (0.66 MB)
- **ci:** `moondeck/docs/`, `moondeck/ci/`, `.github/`, `.vale/`, `.claude/` (0.40 MB)

`moondeck/MoonDeck.md` stays out of scope, as the page where most of these facts are written.

| Run | Cost | Minutes | Tool calls | Claims | Cost per claim |
|---|---|---|---|---|---|
| checks | $14.36 | 18.4 | 50 | 172 | $0.083 |
| moondeck | $8.08 | 12.3 | 60 | 144 | $0.056 |
| ci | $6.41 | 9.6 | 35 | 90 | $0.071 |
| Review | $3.28 | 6.0 | 19 | 57 checked | $0.058 |
| **Total** | **$32.13** | | | 406 | |

| | Sampled | Correct | Partly | Wrong | Citation not showing it | Home wrong |
|---|---|---|---|---|---|---|
| checks | 21 | 19 | 2 | 0 | 0 | 0 |
| moondeck | 22 | 21 | 1 | 0 | 0 | 1 |
| ci | 14 | 11 | 3 | 0 | 0 | 2 |

Every citation resolves. The six partly right claims carry a dated number or one detail too many: `chk-065` counts the tools MoonDeck.md lists as four where it lists three, `chk-041` puts the schema first where two rules run before it, `deck-002` names a `needs_host` key where `--host` follows any set host, `ci-090` says every listed module has a test where Noise2DEffect has none, `ci-065` repeats a stale 143 where 264 test files include `doctest.h`, and `ci-024` says each Vale rule quotes the standard where two paraphrase it. The three wrong homes (`deck-074`, `ci-024`, `ci-028`) are claims homed in a doc whose fact a code comment states, so all 21 gaps stand.

The checks researcher wrote a copy of its answer outside the repository, in the orchestrator's scratch folder; the prompt now forbids a write anywhere.

## Services, util and the web UI

- **services:** `src/core/services/`, `src/main.cpp`, `src/module_types.*`, `src/core/sources.cmake` (20 files, 0.31 MB)
- **util:** `src/core/util/` (53 files, 0.32 MB)
- **ui:** `src/ui/` without its vendored Prism (13 files, 0.86 MB)

| Run | Cost | Minutes | Tool calls | Claims | Cost per claim |
|---|---|---|---|---|---|
| services | $4.11 | 5.5 | 29 | 76 | $0.054 |
| util | $8.47 | 14.6 | 62 | 133 | $0.064 |
| ui | $8.56 | 10.6 | 40 | 175 | $0.049 |
| Review | $3.66 | 7.2 | 21 | 49 checked | $0.075 |
| **Total** | **$24.80** | | | 384 | |

| | Sampled | Correct | Partly | Wrong | Citation not showing it | Home wrong |
|---|---|---|---|---|---|---|
| services | 12 | 11 | 1 | 0 | 1 | 0 |
| util | 20 | 17 | 3 | 0 | 0 | 2 |
| ui | 17 | 16 | 1 | 0 | 1 | 1 |

One citation fails: `ui-005` cites the generated, ignored `src/ui/ui_embedded.h` and is removed from its file. The five partly right claims each carry one wrong detail: `svc-035` says the infrared service hides the Set kind, where it keeps it and relabels it; `util-004` puts the one-space limit on every flat reader, where `parseInt` skips any whitespace through `strtol`; `util-051` says an unread feed is never fetched, where registering it fetches once; `util-057` lists six `ActiveInstance` seats and misses the one in `Drivers`; `ui-169` calls the status bar 56 px, where it is 44 px and 56 adds the padding. Two citations land beside their fact (`svc-035`, and `ui-170`, whose sentence sits at `mooncore.md:87`). The three wrong homes (`util-040`, `util-051`, `ui-052`) are facts a comment or page already states.

## MoonLive, effects and release

- **moonlive:** `src/core/moonlive/`, `src/light/moonlive/`, `moonlive/`, `moondeck/moonlive/` (106 files, 0.42 MB)
- **effects:** `src/light/effects/`, `src/light/powerfunctions/` (81 files, 0.78 MB), told to favor what holds across effects over one claim per header
- **release:** `moonbase/`, `mooninstaller/`, `mooncloud/`, `moondeck/build/` (60 files, 0.65 MB)

| Run | Cost | Minutes | Tool calls | Claims | Cost per claim |
|---|---|---|---|---|---|
| moonlive | $5.87 | 7.7 | 49 | 89 | $0.066 |
| effects | $6.04 | 8.8 | 68 | 85 | $0.071 |
| release | $9.75 | 14.6 | 45 | 133 | $0.073 |
| Review | $4.68 | 8.3 | 27 | 57 checked | $0.082 |
| **Total** | **$26.34** | | | 307 | |

| | Sampled | Correct | Partly | Wrong | Citation not showing it | Home wrong |
|---|---|---|---|---|---|---|
| moonlive | 16 | 15 | 1 | 0 | 1 | 0 |
| effects | 12 | 12 | 0 | 0 | 0 | 0 |
| release | 29 | 27 | 2 | 0 | 2 | 2 |

One citation fails: `rel-116` cites lines past the end of `mooninstaller/devices.js` and is removed from its file. The three partly right claims: `live-084` repeats the contract page's rule that the Xtensa map holds only registers a `call8` keeps, where the map holds `a8` to `a11` and saves them around each call; `rel-019` names two routes both images serve, where `boot-app` and `cancel` are MoonBase's alone; `rel-034` calls a version comment stale, where the app strips the build suffix before it compares. The two wrong homes (`rel-019`, `rel-023`) are facts a test name or a comment states, so 19 of the 21 gaps stand.

## Findings for the product owner

### Bugs

- **A layout made from the picker's template fails its first compile** (`live-068`). The template in `src/light/moonlive/MoonLiveLayout.h:25-27` writes `for (y = 0; …)` without the `int` the compiler requires, and no test compiles it; the service template is the only one a test compiles.
- **`disasm.py` cannot build its emitter** (`live-069`). `moondeck/moonlive/emit_isa.cpp:70` names `CompileResult::kStringPool`, where the compiler has `kMaxStrings`, and its default script predates the class form (`live-070`).

### Security

- **MoonDeck takes commands from any host on the network** (`deck-016`). It binds every interface (`moondeck.py:2820`) and serves `/api/run` and `/api/ota` with no credential, so a machine on the same LAN starts any configured script, a flash included. Fixed on this branch: the LAN still reads the console and pulls firmware, and a POST answers this machine alone, from a page MoonDeck served (`_may_act`, pinned by `test_moondeck_access.py`). The claim is removed from its file, since the fix makes it false.

### Behavior

- **`preview_health` cannot see a preview on current firmware** (`deck-090`). It connects to `/ws` and never sends the `0x51` request, where the firmware serves preview frames on `/wsp` after that request, as `_preview_ws.py` does. Its verdict needs a device run to confirm.

### Docs and comments that disagree with the code

Each confirmed on both sides by a reviewer or in the files.

- `docs/explanation/architecture/moonmodule.md:100` names `onBuildState()` and `teardown()`, where `MoonModule.h` has `prepare()` and `release()`. The fix is on the `docs-match-code` branch, pull request #163; the rest are on every branch.
- `docs/reference/performance.md:41`: `Correction` holds a channel-order table, where it holds per-role offsets.
- `docs/reference/build-system.md:21` and `:25`: `app_main() + Ethernet init` and `mm_main(keepRunning, gridW, gridH)`, where `main.cpp` initializes NVS and `mm_main` takes the run flag and the HTTP port.
- `src/light/drivers/ParallelLedDriver.h:60`: the peripheral gating in `main.cpp`, where it sits in `module_types.cpp:149-165`.
- `src/platform/esp32/platform_config.h:254`: both app slots reserved, where every MoonBase table holds a factory slot and one `ota_0`.
- `esp32/sdkconfig.defaults.eth-spi:6-9`: the filename flips the Ethernet build, where `build_esp32.py` reads each fragment's `CONFIG_ETH_USE_*` lines.
- `moondeck/build/build_esp32.py:101-110`: a `target == esp32p4` rule, where `idf_component.yml` gates the co-processor components on `MM_P4_WIFI`.
- `esp32/sdkconfig.defaults:24`: only the S3 variants override the partition table, where every shipping variant does.
- `esp32/sdkconfig.defaults.wt32eth01`: in no firmware, and sets `CONFIG_ETH_PHY_INTERFACE_RMII`, a symbol IDF v6 lacks.
- `moondeck/MoonDeck.md:152`: `check_specs` compares control names with the header, where it checks help.json's reading, source links and `docPath` anchors.
- `moondeck/MoonDeck.md:190`: `check_docgen`'s roots without `teensy`, which the code and the standard include.
- `moondeck/MoonDeck.md:255`: the gate passes `--no-live-capture`, where the commit process runs `collect_kpi.py --commit` alone.
- `moondeck/MoonDeck.md:326-327` and `check_module.py:15-18`: three and four tools, where `check_module` runs six.
- `moondeck/MoonDeck.md:899` and `build_docs.py:79-81`: `--strict` is a local audit and warnings let the build succeed, where CI and the commit process run `--strict` as the gate.
- `moondeck/MoonDeck.md:1031`: `preview_health` always exits 0 and reads `/ws`, where it exits 1 on a device that is not smooth and the preview is on `/wsp`.
- `moondeck/check/check_specs.py:134` and `:221`: modules registered in `main.cpp`, where the code reads `module_types.cpp`.
- `moondeck/check/check_esp32_built.py:53-54` and `collect_kpi.py:562-563`: point at `CLAUDE.md` sections that moved to the MoonWalk processes.
- `moondeck/moondeck.py:258-260`: a device without a MAC is shown while online, where discovery drops it.
- `moondeck/moondeck.py:686-691`: saving strips the device name and firmware, where only the module list is stripped.
- `.github/codeql-config.yml:9`: 143 test files include `doctest.h`, where 264 do.
- `.claude/workflows/write-behaviour-tests.js:9-11`: 24 modules without a test, where 23 have one.
- `src/core/util/JsonUtil.h:12`: optional whitespace after the colon, where `parseString` and `parseBool` take none or exactly one space.
- `src/core/module/MoonModule.h:203`: the name length fits the longest type name, where `ImprovProvisioning` and `TorontoBarGourds` are cut to 15 characters.
- `src/core/util/sha256.h:18`: the one use is the MoonStats installation id, where `MoonCloudModule` computes it and MoonStats reads it.
- `src/core/util/PinList.h:17`: `parseDottedQuad` in `Control.h`, where it lives in `Ipv4.h`.
- `src/core/util/ImprovFrame.h:22` and `moondeck/build/improv_provision.py:170`: `test/test_improv_frame.cpp`, where the test is `test/unit/core/unit_ImprovFrame.cpp`.
- `src/core/util/MpegTs.h:175`, `math16.h:209` and `oscillators.h:80`: a doc comment cut off mid-sentence.
- `docs/moonmodules/core/ui.md:91`: the enabled toggle shows ✓ and ○, where it shows ⏻.
- `docs/moonmodules/core/ui.md:208` and `:211`: `mm_cardsWidth` and `mm_helpConsent`, which the code never writes, where it writes `mm_helpMapConsent`, `MoonLight.preview.v1`, `mm_cam`, `mm_preview_dot` and `mm_walked`, none listed.
- `src/ui/preview3d.js:3-9`: three wiring points and one stored key, where the module exports eleven members and stores three keys.
- `src/ui/install-picker.js:873-876` and `install-picker-devices.js:6-14`: no bundler and the file embedded verbatim, where esbuild bundles `app.js` and its imports.
- `docs/explanation/architecture/mooncore.md:87` and `src/ui/app.js:1`: two imported modules, no build tools, one file, where `app.js` imports six and esbuild bundles them.
- `src/core/moonlive/register-and-slot-contract.md:34-36` and `moonlive_asm_xtensa.cpp:105`: the Xtensa map holds only registers `call8` keeps, and maps onto `a14` and `a15`, where it ends at `a11` and saves `a8` to `a11` around each call.
- `src/platform/esp32/moonlive_asm_xtensa.h:26`: the assembler is a stack local, where it is placed on the heap.
- `docs/explanation/architecture/moonbase.md:56`: MoonBase shares three headers, where its build names four and its source includes a fifth.
- `moondeck/build/build_esp32.py:6`, `generate_manifest.py:19` and `generate_firmwares.py:17`: a "Firmware vs board" section of the architecture index, which does not exist; the words live in `mooninstaller.md#the-three-words`.
- `docs/reference/build-system.md:74`: `setup_esp_idf.py` holds the IDF pin, where `build_esp32.py` holds it and `setup_esp_idf.py` imports it.
- `moondeck/build/flash_esp32.py:172-174`: 921600 baud is not the default, where it is.
- `moondeck/MoonDeck.md:1068`: the default IDF install pulls the classic toolchains only, where `setup_esp_idf.py` installs all.
- `moondeck/MoonDeck.md:1150-1162`: `release_version` moves `library.json`, where it also moves the OpenAPI version.
- `moondeck/build/improv_provision.py:211-213`: the browser installer sends open-loop, where it reads each reply and resends.
- `docs/explanation/architecture/mooninstaller.md:46`: a `SET_BOARD` command, which no code has; the device model rides in the System document.
- `mooncloud/worker.js:5`, `schema.sql:3` and `README.md:9`: two endpoints and one table, where the worker routes four and creates three tables.
- `mooncloud/wrangler.toml:24`: the firmware hardcodes the `stats.moonmodules.org` route, where it compiles in the `workers.dev` host.

### MoonAtlas itself

- **Three researchers refreshing a changed tree at once wrote into one temporary file**, and `show_atlas` failed with a disk I/O error. Each writer now has a file of its own.
- **The graph sees no Python import.** `gen_atlas` follows C++ `#include` only, so `impact` and `tests` answer nothing for a MoonDeck or CI script; each researcher in that area read the files directly. Whether `show_atlas` shortens a researcher's reading is measured once the graph covers the area being read.

## What it says about the remaining areas

Every area of code is researched: four rounds, 6.59 MB, $118.35 with review, about $18 per MB, 1,467 claims, three removed for a citation that does not resolve and one made false by this branch's MoonDeck fix, and no sampled claim of 215 is wrong. Tests and `docs/work` (7.3 MB) are mostly scenario data, test bodies and plans, which the graph already joins through `tests`, `proves` and `mentions`; a researcher there would restate them, so they are candidates to skip.
