# MoonAtlas example: adding a picture source

A new maintainer is asked to add animated GIF as a picture source. They know only that the effect is called Picture. Four queries of `show_atlas` take them from that name to the file to change, the page to update and the tests to keep green. Output from the tree on 2026-10-29.

## What it is

```
$ uv run moondeck/docs/show_atlas.py what PictureService
module:PictureService: The picture the Picture effect samples onto the lights, from one source at a time.
  defined_in -> src/core/services/PictureService.h
  documented_by -> docs/moonmodules/core/services.md#picture
  has_control -> control:PictureService.fps
  has_control -> control:PictureService.height
  has_control -> control:PictureService.noiseFloor
  has_control -> control:PictureService.source
  has_control -> control:PictureService.staleMs
  has_control -> control:PictureService.width
  is -> PictureService  src/core/services/PictureService.h:23
  tests <- test/unit/core/unit_PictureService.cpp
  tests <- test/unit/light/unit_PictureEffect.cpp
class:PictureService  src/core/services/PictureService.h:23
  derives -> MoonModule  src/core/module/MoonModule.h:59
  defines <- src/core/services/PictureService.h [23]
  is <- module:PictureService
  mentions <- docs/work/present/Plan-20261013 - Streams, every frame from outside (11 of 13).md
```

The picture handler is a service in core, separate from the effect. Its `source` control picks where the picture comes from, so a GIF source joins that list. A plan in flight, Streams, mentions the service, and the maintainer reads it before designing, since animated pictures may already be on its roadmap.

## Why it is shaped that way

```
$ uv run moondeck/docs/show_atlas.py why PictureService
documented by (1)
  docs/moonmodules/core/services.md#picture
explained in code: none
```

One page to read. The header has no `/// ##` sections, so its reasons are the comments on each member. At line 32 the header holds `enum Source { kTestPattern, kUsbCapture, kSourceCount }`, where a third source goes.

## What the change touches

```
$ uv run moondeck/docs/show_atlas.py impact PictureService
included by (2)
  src/light/effects/PictureEffect.h
  src/module_types.cpp
unit tests (2)
  test/unit/core/unit_PictureService.cpp
  test/unit/light/unit_PictureEffect.cpp
scenarios: none
MoonWalk steps: none
docs (2)
  docs/moonmodules/core/services.md#picture
  docs/work/present/Plan-20261013 - Streams, every frame from outside (11 of 13).md
claims (10)
  … svc-015, svc-016 …
  docs/work/present/atlas/areas/services-fable.json#svc-065  A source writing from its own task gets a front and a back picture swapped under a TryLock, and when the second does not fit a free block with the heap reserve intact it degrades to one picture locked for the decode, so frames skip but never tear.
  … svc-066 …
  docs/work/present/atlas/areas/services-fable.json#svc-067  The capture task decodes into the size and noise floor the pictures were prepared with, bufW_, bufH_ and floor_, never the width, height or noiseFloor controls, which a write may already have changed under it.
  docs/work/present/atlas/areas/services-fable.json#svc-068  A picture older than staleMs is replaced by an empty frame with the generation bumped, so an effect's redraw guard sees the change and the lights go out rather than staying lit from a frozen frame.
  … svc-069, svc-070, svc-076, util-057 …
```

The blast radius is two includers, two unit tests and one doc section. `scenarios: none` is a finding of its own: no scenario runs the picture path end to end, so a GIF source brings a unit test, and the first scenario for the path is worth considering. The claims name what a new source must respect: decode from its own task into the double buffer (`svc-065`), into the size the pictures were prepared with rather than the live controls (`svc-067`), and let the stale drop put the lights out when it stops (`svc-068`).

## Which tests stay green

```
$ uv run moondeck/docs/show_atlas.py tests PictureService
test/unit/core/unit_PictureService.cpp
test/unit/light/unit_PictureEffect.cpp
```

## A change deeper in core

The same four queries scale. `impact MoonModule.h` reports 189 includers, 200 unit tests, 36 scenarios and 28 MoonWalk steps, which tells the maintainer to run the whole suite and the scenarios rather than one test. `rules` lists the standards sections and the checks that enforce each.

## Where the graph stops

- **Calls.** USB capture decodes MJPEG behind a `platform::videoCapture` call, and the graph holds file and class edges but no call edges, so the maintainer finds the decoder with a search. That is where a GIF decoder's place is decided.
