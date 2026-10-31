# Plan: HUB75 on a Raspberry Pi through rpi-rgb-led-matrix

Plan of 2026-10-30, a Feature, for approval. MoonLight on a Raspberry Pi drives HUB75 panels from the Pi's own header, linking hzeller's [rpi-rgb-led-matrix](https://github.com/hzeller/rpi-rgb-led-matrix) for the panel timing, so effects and panels run in one process with no bridge and no receiving card. A Pi 5 then drives walls past what one ESP32 does.

## What exists

- **The bridge works today.** On the bench Pi 5, MoonLight 7.0.0 from the arm64 `.deb` sends DDP over loopback to matrixprism's `matrix-dmx`, which links the library: a 256x192 wall (128x64 panels, chain 2, parallel 3) shows Raymarch, Fluid and VectorBalls composited at 42 fps. It costs a second process, an 8-bit hop and three UDP ports.
- **`Hub75Driver` is ESP32 only.** The light domain encodes binary-coded planes (`Hub75Slots.h`) into the platform's DMA buffer, and the desktop stubs report "HUB75 needs an ESP32-S3, P4 or S31" (`platform_desktop.cpp:2758`).
- **A Pi runs the desktop build**, with no hardware access. [the backlog card](backlog_mirror/platform.md#led-output-from-a-raspberry-pis-own-pins-robwomp-2026-10-11) names HUB75 on a Pi as a second backend after a WS2812 one.
- **Two optional-library precedents.** libcurl links at build time when found (`CMakeLists.txt:216`, `MM_HAVE_CURL`); NDI resolves at run time through `dlopen` (`platform_desktop.cpp:3136`).
- **The library's behavior, measured on the bench Pi 5 (2026-10-30)** with a standalone program against the pinned library on the 256x192 wall:
  - `Options::Validate` refuses 0 rows, 12 PWM bits and multiplexing 99 with a reason, and nothing ends the process.
  - Open, close and reopen within one process works five times running, including RIO to PIO and back.
  - It runs as `dietpi` with the `gpio` group and `CAP_SYS_NICE`, no root. Without the capability it drives the panel but warns that real-time priority failed and colors will flicker.
  - `SwapOnVSync` paces at 89 fps at 11 bits, 101 at 8 bits and 74 on PIO.
- **The library** covers the Pi 1 to 5, and MoonLight's arm64 build the 64-bit ones, a Pi 3, 4, 5 or Zero 2 W: the RP1 backends landed upstream in `d110602` (2026-04-26), RIO by default and PIO on request. It is GPL-2.0-or-later with an explicit GPL-3.0 grant in its README, so it links into MoonLight's GPL-3.0. Debian does not package it. Its own CMake builds only a shared library; its Makefile, the build the README documents, builds `librgbmatrix.a`.

## Design

### The seam sits above the encoding

On an ESP32 the driver encodes and the platform clocks the wire. The library encodes itself, at up to 11 bit planes, from its own refresh thread, so the seam takes an RGB frame:

```cpp
bool rgbMatrixOpen(const RgbMatrixOptions& o);   // false: rgbMatrixLastError() says why
void rgbMatrixPush(const uint8_t* rgb);           // one frame, the newest winning at the next vsync
void rgbMatrixBrightness(uint8_t level);
uint16_t rgbMatrixFps();                          // frames shown in the last second
void rgbMatrixClose();
```

The Linux implementation is one file, `platform_desktop_rgbmatrix.cpp` beside `platform_desktop_audio.cpp`, and the only place the library's headers appear. Every other host keeps the last pushed frame in a heap buffer and reports "needs a Raspberry Pi", so the driver links, runs and is unit-tested on every desktop, as the hard rule asks. Timing and pins are not emulated.

### A driver of its own

`Hub75PiDriver`, beside `Hub75Driver`. Its controls are the library's own catalog, under its names: `mapping` (regular, adafruit-hat, adafruit-hat-pwm, regular-pi1 and the rest), `rows`, `cols`, `chain`, `parallel`, `pixelMapper` (Rotate:90, U-mapper and the rest, which the bench Pi 4's 96x64 sign needs), `multiplexing`, `rowAddrType`, `panelType`, `pwmBits`, `pwmLsbNs`, `ditherBits`, `slowdown`, `scanMode`, `rp1Pio`, `limitRefresh`, and the frames shown each second, `shown`, read-only.

Not a third `Hub75Backend`: `Hub75Driver`'s controls are fourteen pins, `scanRate`, `bitDepth` 2 to 6 and `clockEdge`, and none of them reaches the library, which takes a named GPIO mapping. One driver with two disjoint control sets is two drivers in one file.

Every control change closes the matrix and opens a new one, the [live reconfiguration](../explanation/architecture/moonmodule.md#live-reconfiguration-every-change-applies-on-the-next-frame) rule, which the bench measurement shows the library supports.

### Brightness and the curve go to the library

The driver reports `dimsByTime()`, so MoonLight's brightness reaches `SetBrightness`, and the library's CIE 1931 correction runs at its full bit depth. The driver sends linear 8-bit values. Today's bridge applies CIE before the 8-bit DDP hop and loses steps at the dark end; this path keeps them.

### Refuse bad options with a reason

The library's init paths are named `...OrDie` and end the process. The driver runs `RGBMatrix::Options::Validate` before every open and refuses what it rejects with the reason in its status. Every exit path found during step 2 gets a guard and a test.

### Privileges and threads

- **Runtime options:** `daemon = -1` with `StartRefresh()`, so the library never forks, and `drop_privileges = -1`, so it never changes MoonLight's user.
- **Pi 5:** RIO maps `/dev/gpiomem0` and PIO opens `/dev/pio0`, both owned by the `gpio` group, so MoonLight runs unprivileged once its user is in that group.
- **Pi 3 and 4:** the library maps `/dev/mem` for PWM and its timer. `/dev/gpiomem` reaches only GPIO, and the library then refuses the other registers, so these need root.
- **The refresh thread** asks for `SCHED_FIFO` and pins itself to a core. Unprivileged, that needs `AmbientCapabilities=CAP_SYS_NICE` in the systemd unit, and the library's `isolcpus=3` advice keeps MoonLight's render threads off that core.

### Linking at build time, from a pinned commit

The build fetches the library at a pinned commit (`51d3231`, 2026-09-07, which carries the RP1 work) and builds upstream's own `librgbmatrix.a` through its Makefile, on Linux aarch64 only, which is the one Pi architecture MoonLight publishes. It defines `MM_HAVE_RGBMATRIX` and links statically, so the arm64 `.deb` drives a panel on install. A failed fetch fails the configure, so no arm64 package ships without it. `attributions.md` names the library and its license.

Runtime `dlopen` of `librgbmatrix.so.1` is the alternative, rejected for two reasons. The user would build and install the library themselves. And the C API's option structs are unversioned: they grew the RP1 fields this year, so a library newer or older than the headers MoonLight compiled against misreads its options silently.

## Steps

1. ✅ **The build rule.** The pinned fetch, the static archive and `MM_HAVE_RGBMATRIX` on Linux aarch64, in `CMakeLists.txt`, with the attribution. Built on the bench Pi with GCC 14 and clang 19: the archive compiles with neither `-march` nor `-flto`, and the seam links its objects.
2. ✅ **The seam.** `rgbMatrix*` in `platform.h`, `platform_desktop_rgbmatrix.cpp`, `hasRgbMatrix` per platform, and `unit_platform_rgbmatrix.cpp`. All 2,857 test cases pass on macOS, warning-free under its `MM_NONBLOCKING` checks, and on the bench Pi 5. A program driving the panels through the seam, with a 200 fps producer that never waits:
   - Pi 5, the 256x192 wall: RIO 11-bit at 92 fps, reopened at 8-bit (103) and on PIO (74), a brightness ramp, and 0 rows refused with the library's reason.
   - Pi 4 as root, the 96x64 protest sign: stripe multiplexing, `Rotate:90` folding 64x96 into 96x64, `gpio_slowdown` 2, at 197 fps, the producer's rate.
3. ✅ **The driver.** `Hub75PiDriver.h`, registered behind `hasRgbMatrix`, and `unit_Hub75PiDriver.cpp`. All 2,867 test cases pass on macOS, warning-free, and on the bench Pi 5, the contract survey recording rather than opening panels. On the wall it drives Raymarch, Fluid and VectorBalls at 256x192 in one process, 0.24 ms a frame for the driver; 36 fps from a GCC release build, render-bound, where clang 19 renders Raymarch about 25% slower.
4. ✅ **On hardware.** A package from a container matching the release job (Ubuntu 22.04, gcc-12, `package_desktop.py`) drives the bench Pi 5 wall as `dietpi` in the `gpio` group with `CAP_SYS_NICE`, no warning: RIO at 40 fps, PIO switched on live at 39 fps. The product owner saw the wall and the Pi 4 sign right; all 2,868 test cases pass on macOS and the bench Pi 5.
   - **The library's PIO teardown stalls the next PIO session.** `Rp1PioDeinit` disables the state machine while DMA is in flight, so the next open, in this process or the next, waits forever in `rp1_pio_sm_xfer_data32_user`; a stalled process killed by the kernel resets it, which is why runs alternated. Waiting until the TX FIFO stays empty before disabling fixes it: five consecutive runs as `dietpi`, from GCC and from clang. To carry as a patch on the pinned commit and offer upstream.
   - Earlier explanations of the PIO stall, a user other than root, the thread split, per-frame brightness and clang, came from that alternation, not from them.
   - The seam moved meanwhile to one library thread with a close time limit, and sets brightness only on a change.
   - **PIO stays experimental.** With the patch, PIO toggles live and boots after clean stops, yet once stalled at open after a stop cut short by a second signal. The driver's guide says so and RIO is the default. A library thread that stops answering is marked stuck: later opens fail at once with "restart MoonLight", and exit never joins it. Noted for upstream in `~/Documents/src/upstream-notes/rpi-rgb-led-matrix-pio-teardown.md`.
   - **Every control live**, on the wall: all apply, a canvas change warns of the layout and recovers. One crashed MoonLight: a mapping with fewer parallel chains than asked, which the library aborts on. The seam now refuses it by the library's own table, pinned by `unit_platform_rgbmatrix.cpp` on a Pi, and on a Pi 3 or 4 refuses a loaded onboard sound module, the library's other exit, which only a Pi 3 or 4 with sound loaded can show. `compute-module` left the mapping list, absent from the library's default build, and the `-pi1` mappings with it, for a Pi 1 the arm64 build never runs on.
   - **A reboot** brings the wall back as `dietpi` on its own, at 38 fps.
   - **The Pi 4 sign** through the driver, as root: 96x64 with stripe multiplexing, `Rotate:90`, a 160 ns LSB and slowdown 2, Raymarch, Fluid and VectorBalls at 140 fps, 0.07 ms a frame for the driver.
   - **MoonLight stops cleanly on SIGTERM**, as on SIGINT: `systemctl stop`, Docker and `kill` used to end it with no teardown, freezing a HUB75 panel on a lit row. Three PIO stop-and-boot cycles each exit in under a second.


5. ✅ **Docs.** A Raspberry Pi section in [hub75-panels.md](../reference/hardware/hub75-panels.md#raspberry-pi), holding the status table the driver's header no longer carries; the HUB75 Pi branch of [Set up a driver or service for the hardware](../moonwalk.md#driver-setup) in place of a process of its own; `SupplementaryGroups=gpio`, `CAP_SYS_NICE` and a clean `systemctl stop` in [Keep MoonLight running on a Linux board](../moonwalk.md#run-linux-board); the corrected Pi paragraph of [the LED-driver design study](../design/leddriver.md#led-output-from-a-raspberry-pis-own-pins); SIGTERM and the Linux aarch64 configure in `build-system.md`; the card image. Left to others: the backlog card's HUB75 sentence, edited in the Project, and MoonAtlas claim `plat-033`, on its own branch.

## What comes out

- The HUB75 sentence of the backlog entry "LED output from a Raspberry Pi's own pins": this plan builds it. The WS2812 half stays.
- The stale Pi lines in [the LED-driver design study](../design/leddriver.md#led-output-from-a-raspberry-pis-own-pins): the library covers the Pi 5, and PioMatter is no longer the only proof.
- No `src/platform/rpi/` directory: the Pi stays the Linux desktop build with one more file.
- The DDP bridge stays matrixprism's, for walls fed from another machine.

## Decisions

Taken by the product owner on 2026-10-30:

1. **Link at build time**, not `dlopen` at run time.
2. **A driver of its own**, not a third backend of `Hub75Driver`.
3. **Brightness and the curve in the library**, not in MoonLight's `Correction`.
4. **Handing the output to another system has precedent.** `PanelCardDriver` hands it to a ColorLight receiving card and `NdiDriver` to the NDI library, so handing it to this library follows the same pattern.
