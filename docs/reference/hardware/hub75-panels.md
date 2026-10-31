# Driving a HUB75 panel

A HUB75 LED panel can be driven straight from an ESP32's pins with the [HUB75 driver](../../moonmodules/light/drivers.md#hub75), from a Raspberry Pi's header with the [HUB75 Pi driver](../../moonmodules/light/drivers.md#hub75pi), or over Ethernet through a receiving card with [Panel Card](../../moonmodules/light/drivers.md#panelcard). Board, panel and settings are below, in that order, and the Pi has [its own section](#raspberry-pi).

## Which path

A receiving card earns its place above roughly **16,384 pixels**. Below that, a card is a Windows tool to configure and a dedicated Ethernet link to run, for a panel the board could drive itself. The HUB75 driver is the small end of that split; for a card, see [Driving LED panels with a receiving card](panel-cards.md). A Raspberry Pi sits between them: its header drives a wall a card would otherwise carry, such as six 128x64 panels, from the process that renders it.

A [MoonHub75](https://moonmodules.org/projects/hardware/#moonhub75-pcb) drives a 64x64 1/32-scan panel at 6-bit and 1,508 Hz. Larger walls and other panel families wait on reports: the `refresh` reading plus your geometry makes one useful.

## Which board

On an ESP32 the chip needs an LCD_CAM or a Parlio block: an S3, a P4 or an S31. The classic ESP32 has neither, and its 13 usable output GPIOs are all a 1/16 port needs, which leaves nothing for a strand, a button or a microphone.

Three boards have published pin maps, which the `board` select fills in:

- **MoonHub75**, a Lilygo T7-S3 on a passive adapter. It also carries an INMP441 microphone socket on IO10/11/12, so an audio-reactive effect and a panel run together.
- **Adafruit MatrixPortal S3.** Three of its lines sit on GPIO 35 to 37, which the octal-PSRAM images hold, so it runs the `esp32s3-n8r2` image, built for its quad-PSRAM N8R2 part.
- **Waveshare ESP32-S3 RGB Matrix**, SKU 34422. The Waveshare ESP32-S3-Matrix is a different product, with an onboard 8x8 matrix and no HUB75 connector.

A published map hides the pin rows, since those lines are soldered; they stay bound and still drive the panel. The generic per-chip sets and Custom show them. A line on a pin the chip wires to flash or PSRAM is refused before init, with the line named. Free pins per chip: [GPIO usage](gpio-usage.md).

!!! tip "The one naming trap"
    A panel's ribbon names its color lines R1/G1/B1 (upper half) and R2/G2/B2 (lower half). Some board docs call the same pairs R0/G0/B0 and R1/G1/B1.

## Settings the panel cannot tell you

Steps: [Set up a driver or service for the hardware](../../moonwalk.md#driver-setup), its HUB75 choice.

- **`scanRate`** is read off the panel's label or listing, not calculated: two panels of the same size can scan differently. A 1/32-scan panel needs the `e` line; on 1/16 that pin is free.
- **`peripheral`** matters on a P4, which has one LCD_CAM and one Parlio: a board already driving LED strips from one needs the panel on the other.
- **`clockEdge`**: every pixel one column over means the other edge.

## Depth, refresh and brightness

Every bit plane is a full scan, so `bitDepth` costs refresh and memory linearly. One 64x64 panel at 1/32 scan is 16,384 bytes a frame at 4-bit and 24,576 at 6-bit. Brightness on a HUB75 panel is time, not amplitude, so lowering it shortens each plane's lit time and keeps every color level. The price is top brightness: full white lights about a third of the time at 6-bit.

On the bench (2026-10-06, the MoonHub75's panel on LCD_CAM) 4-bit refreshes at 2,157 Hz, against the 2,441 Hz its `(height / 2) × bitDepth` line times predict before latch overhead. 6-bit costs about 2 ms more render tick, about 57 against 64 fps.

If a panel flickers, `refresh` is the measured rate to report.

**A panel takes its own 5 V supply on its power connector.** A lit row draws all its columns at once, so the peak current does not fall with brightness: a 64x64 panel at full red and blue pulls about 2.6 A. A supply that sags under it, such as a controller board's USB, drops the blue and green LEDs first, which need about 3 V where red needs 2, so a color with much red in it shows as red or orange.

## Raspberry Pi

The HUB75 Pi driver drives panels from a Pi's header through Henner Zeller's [rpi-rgb-led-matrix](https://github.com/hzeller/rpi-rgb-led-matrix), which MoonLight's arm64 package carries, so a 64-bit Pi, a 3, 4, 5 or Zero 2 W, needs nothing else installed. On the bench a Pi 5 drives six 128x64 panels as one 256x192 wall at about 40 rendered frames a second, and a Pi 4 a 96x64 sign at 140. Steps: [Set up a driver or service for the hardware](../../moonwalk.md#driver-setup), its HUB75 Pi choice.

- **Wiring.** The `mapping` select holds the library's GPIO mappings by name: `regular` for a passive adapter, `adafruit-hat` or `adafruit-hat-pwm` for Adafruit's bonnet and HAT, `classic` for the oldest adapters. The library's [wiring guide](https://github.com/hzeller/rpi-rgb-led-matrix/blob/master/wiring.md) shows each one's pins. A mapping drives a fixed number of parallel chains, `regular` three and the Adafruit ones one.
- **`slowdown`** stays on `auto`, where the library picks it per model: 2 on a Pi 4, 1 on a Pi 5.
- **The canvas.** The panels form `cols` times `chain` across and `rows` times `parallel` down, reshaped by `pixelMapper`, such as `Rotate:90` for a wall on its side. Size the layout to the canvas the status names.
- **Permissions.** A Pi 5 reaches the panels through `/dev/gpiomem0`, which the `gpio` group opens, so MoonLight runs as an ordinary user in that group. A Pi 3 or 4 maps `/dev/mem`, which takes root. The library's refresh thread wants real-time priority, `CAP_SYS_NICE`; the service unit grants both: [Keep MoonLight running on a Linux board](../../moonwalk.md#run-linux-board). The library also advises `isolcpus=3` in `/boot/firmware/cmdline.txt`, which keeps MoonLight's render threads off the core its refresh runs on.
- **Onboard sound on a Pi 3 or 4.** It shares the PWM block the library times the panels with, so the panels refuse while it is loaded: `dtparam=audio=off` in `/boot/firmware/config.txt`. Audio comes in over USB or [audio sync](../audio-input.md#driving-a-fleet-of-devices) instead. A Pi 5's sound does not collide.
- **PIO is experimental.** `rp1Pio` clocks a Pi 5's panels through the RP1's PIO block in place of RIO. On the bench it opens, runs and reopens, yet it has stalled at an open, after which MoonLight needs a restart, so an installation keeps RIO.

| The status says | It means |
|---|---|
| `256x192, 11-bit` | Driving: the canvas and the color depth, with `, PIO` when `rp1Pio` is on |
| `layout 128x64, panels 256x192` | Size the layout to the panels |
| `no access to /dev/gpiomem0: add the user to the gpio group` | Permissions, above; on a Pi 3 or 4 it names `/dev/mem` and root |
| `no /dev/pio0 on this system` | The kernel's PIO driver is missing, so keep RIO |
| `the adafruit-hat mapping drives at most 1 parallel chain` | Fewer `parallel`, or a mapping with more chains |
| `the Pi's onboard sound is loaded, which the panels cannot share: set dtparam=audio=off` | Onboard sound, above |
| `no real-time priority, so colors may flicker: grant CAP_SYS_NICE` | The refresh thread runs unprioritized |
| `another driver drives the panels` | One HUB75 Pi driver per Pi |
| `the panels did not open in time` | The library stalled while opening, on PIO |
| `the panels' library is stuck in a transfer: restart MoonLight` | After such a stall; switch `rp1Pio` off and restart |
| `needs a Raspberry Pi build with rpi-rgb-led-matrix` | This build has no library: a Pi's arm64 package has it |
| `not a Raspberry Pi` | Another arm64 board, or a container that hides the board's model |
| `the library refused the panels; its reason is in the log` | The library declined the options as a whole; MoonLight's log carries its words |
| `not enough memory for the frame` or `not enough memory for one light of this fixture` | Smaller panels, or a narrower fixture profile |
| any other message | An option the library refuses, in its own words, such as an odd number of `rows` |
