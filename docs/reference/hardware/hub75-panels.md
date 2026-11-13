# Driving a HUB75 panel

A HUB75 LED panel can be driven straight from the board's pins with the [HUB75 driver](../../moonmodules/light/drivers.md#hub75), or over Ethernet through a receiving card with [Panel Card](../../moonmodules/light/drivers.md#panelcard). Board, panel and settings are below, in that order.

## Which of the two paths

A receiving card earns its place above roughly **16,384 pixels**. Below that, a card is a Windows tool to configure and a dedicated Ethernet link to run, for a panel the board could drive itself. The HUB75 driver is the small end of that split; for a card, see [Driving LED panels with a receiving card](panel-cards.md).

A [MoonHub75](https://moonmodules.org/projects/hardware/#moonhub75-pcb) drives a 64x64 1/32-scan panel at 6-bit and 1,508 Hz. Larger walls and other panel families wait on reports: the `refresh` reading plus your geometry makes one useful.

## Which board

The chip needs an LCD_CAM or a Parlio block: an S3, a P4 or an S31. The classic ESP32 has neither, and its 13 usable output GPIOs are all a 1/16 port needs, which leaves nothing for a strand, a button or a microphone.

Three boards have published pin maps, which the `board` select fills in:

- **MoonHub75**, a Lilygo T7-S3 on a passive adapter. It also carries an INMP441 microphone socket on IO10/11/12, so an audio-reactive effect and a panel run together.
- **Adafruit MatrixPortal S3.** Three of its lines sit on GPIO 35 to 37, which the octal-PSRAM images hold, so it runs the `esp32s3-n8r2` image, built for its quad-PSRAM N8R2 part. It has no microphone; one wires to the free header pins, A0 to A4 and TX/RX, which its [pinout](../../assets/deviceModels/esp32-s3-n8r2-adafruit-matrixportal-pinout.jpg) shows, and takes an Audio service set to those pins.
- **Waveshare ESP32-S3 RGB Matrix**, SKU 34422. The Waveshare ESP32-S3-Matrix is a different product, with an onboard 8x8 matrix and no HUB75 connector.

A published map hides the pin rows, since those lines are soldered; they stay bound and still drive the panel. The generic per-chip sets and Custom show them. A line on a pin the chip wires to flash or PSRAM is refused before init, with the line named. Free pins per chip: [GPIO usage](gpio-usage.md).

A Raspberry Pi 5 drives panels through its RP1 PIO instead — see below.

!!! tip "The one naming trap"
    A panel's ribbon names its color lines R1/G1/B1 (upper half) and R2/G2/B2 (lower half). Some board docs call the same pairs R0/G0/B0 and R1/G1/B1. A panel that lights every pixel in the wrong hue has its color lines in another order: swap the pins, not the panel.

## Raspberry Pi 5 (RP1 PIO)

A Pi 5 drives HUB75 panels straight from its 40-pin header, through one RP1 PIO state machine and [piolib](https://github.com/raspberrypi/utils/tree/master/piolib) — no receiving card, no ESP32. Pick `Pi PIO` in the driver's `peripheral` select; it is offered only on a build with piolib linked.

**The board.** The wiring is fixed: the active 3-channel board's (hzeller `regular` mapping), three HUB75 line sets on GPIO 2..27 with shared address, clock, latch and output-enable. The card carries no pin controls and no board select — the wiring is soldered, and the state machine drives all three chains in lockstep from one 32-bit word a pixel clock. A 1/32-scan panel needs the `e` line, GPIO 15, and the board's E jumper set to match the panel (HUB75 pin 4 or 8).

**Geometry.** The layout sets it, as on the matrix output: `rows` and `cols` are one panel's, `chain` the panels cabled in series, and the layout's lights, cut into whole panels, are the panels. `parallel` follows, bounded by the board's three chains — chain N is the Nth strip down the canvas, panels in index order along it. The defaults are a 256x192 wall: 128x64 panels, 2 to a chain, 3 chains stacking to 256x192 at 1/32 scan and 6-bit. A layout of another count, or off plain rows, is the warning; a geometry that does not cable is the refusal with the way out.

**Refresh.** One state machine shifts one word a pixel clock, so the frame is `bitDepth × address steps × chain width` clocks: 203 Hz on the 256x192 wall at the default 10 MHz pixel clock and 6-bit. `pixelClock` (1–40 MHz) trades refresh against panel tolerance; `latchBlank` (0–8 words) steadies a panel that ghosts. `shown` is the measured refresh. The arrangement rates the geometry messages quote come from the same pixel-clock arithmetic.

**Permissions.** The engine opens `/dev/pio0`, which needs read/write access: run MoonLight as root, or as a user in the `gpio` group (plus piolib's udev rule if the node is root-owned). A kernel from 6.18.39 up to 6.18.60 stalls a PIO reopen, so the open is refused outright on those kernels — the PIO output has no fallback — with the reason in the status line; 6.18.60 and later serve PIO.

**Building.** The PIO output builds on every host, but the real backend needs piolib: build and install it from [raspberrypi/utils](https://github.com/raspberrypi/utils) (`cmake -S piolib -B build && cmake --build build && sudo cmake --install build`), or point CMake at an install with `-DMM_HUB75_PIO_PIOLIB_DIR=/path/to/piolib-install`. Without it the build still succeeds, but the peripheral is never offered and an open fails with `needs a Raspberry Pi build with piolib`.

!!! warning "Unverified on a panel"
    The PIO output is implemented and unit-tested on a desktop host, but nothing has been displayed on a real panel yet: the PIO program on RP1 silicon, the 3-chain strip mapping, DMA throughput at 40 MB/s, and panel compatibility are all pending on-device testing. First light should be a single panel (128x64 layer, chain 1), then the wall.

## Which panel

The driver speaks to plain shift-register driver chips, such as the MBI5124, where the board makes the gray levels as bit planes and the latch takes one clock. An S-PWM chip (the ICN1065, the DP3264S and their relatives) makes its own gray and expects a register setup the driver does not send, so it stays dark, and a chip that wants several clocks under the latch, such as the ICN2038S, is not driven either. The driver chip's marking tells the families apart: the advertised gray depth, refresh rate, pitch, size and scan rate do not, so ask the seller for the marking. [ledwall](https://github.com/siegeld/ledwall) drives both families from a Colorlight receiving card's FPGA, and its panel notes explain the difference.

## Settings the panel cannot tell you

Steps: [Set up a driver or service for the hardware](../../moonwalk.md#driver-setup), its HUB75 choice.

- **`scanRate`** is read off the panel's label or listing, not calculated: two panels of the same size can scan differently. A 1/32-scan panel needs the `e` line; on 1/16 that pin is free.
- **`peripheral`** matters on a P4, which has one LCD_CAM and one Parlio: a board already driving LED strips from one needs the panel on the other.
- **`clockEdge`**: every pixel one column over means the other edge.

## Depth, refresh and brightness

Every bit plane is a full scan, so `bitDepth` costs refresh and memory linearly. One 64x64 panel at 1/32 scan is 16,384 bytes a frame at 4-bit and 24,576 at 6-bit. Brightness on a HUB75 panel is time, not amplitude, so lowering it shortens each plane's lit time and keeps every color level. The price is top brightness: full white lights about a third of the time at 6-bit.

On the bench (2026-10-06, the MoonHub75's panel on LCD_CAM) 4-bit refreshes at 2,157 Hz, against the 2,441 Hz [ceiling](hub75-refresh.md#how-drivers-set-the-lsb) its shift clock predicts. What a longer chain or deeper color costs: [HUB75 refresh rates](hub75-refresh.md). 6-bit costs about 2 ms more render tick, about 57 against 64 fps.

If a panel flickers, `refresh` is the measured rate to report. Flicker only in the dark areas, while bright ones hold still, is a refresh rate too low: a dim pixel is lit for one short pulse a refresh, and the eye catches the short pulse first.

**A panel takes its own 5 V supply on its power connector.** A lit row draws all its columns at once, so the peak current does not fall with brightness: a 64x64 panel at full red and blue pulls about 2.6 A. A supply that sags under it, such as a controller board's USB, drops the blue and green LEDs first, which need about 3 V where red needs 2, so a color with much red in it shows as red or orange. A 128x64 panel measured about 4 A at full white, and panels differ, so a supply is sized from each panel's rated maximum current times the panel count, and the voltage is checked at the far panel's connector under a white frame. A picture that holds on dark content and breaks up on bright content is a power fault, whatever it looks like.
