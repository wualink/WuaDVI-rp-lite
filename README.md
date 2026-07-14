# WuaDVI-rp-lite

Official firmware for the **RP2354B display engine** of the WuaDVI board
(Wualink, member of Wualabs — [wualabs.com](https://wualabs.com)).

The RP2354B acts as a **dumb DVI framebuffer renderer**: it generates the
HDMI/DVI signal with PicoDVI and blits whatever the ESP32-C3 application
processor sends over SPI. No UI logic lives here — the RP knows nothing about
widgets, layouts or events; it only copies pixels to a screen.

On boot it shows a branded splash (organization, product, firmware version,
resolution) and answers a small SPI control protocol that lets the ESP32 read
the firmware version — and re-flash this firmware through the RP2350 ROM UART
boot if it is outdated — before handing the screen over.

---

## System Architecture

```
                 control link · SPI 1 MHz · 32 B commands / 8 B PONG
          ┌──────────────────────────────────────────────────────────┐
          │    PING → version / mode / resolution                    │
          │    DISPLAY_START → switch to framebuffer mode            │
          ▼                                                          │
┌─────────────────────┐        SPI 25 MHz (mode 1)        ┌──────────┴─────────┐
│  ESP32-C3 (master)  │ ────────────────────────────────► │  RP2354B (slave)   │
│  LVGL renders UI    │    dirty rects, fixed-size DMA    │  PicoDVI scanout   │
│  → dirty rects      │    [12 B header + pixel payload]  │  blits rects into  │
└─────────────────────┘                                   │  the framebuffer   │
                                                          └─────────┬──────────┘
                                                                    │ HDMI
                                                                    ▼
                                                                 Monitor
```

## Boot sequence

1. PicoDVI starts (`pio_set_gpio_base(pio0, 16)` first — HDMI GPIOs 32-38 sit
   above the default PIO window).
2. The **boot splash** is drawn: Wualink / Wualabs branding, product name
   (`WuaDVI v1.0`), firmware version, active resolution and an animated
   `Loading...` caption.
3. The **control link** (SPI1 slave, polled) starts answering `PING` with an
   8-byte PONG: firmware version, current mode and resolution id. Mode is
   `SPLASH` first, then `READY` once the minimum splash time
   (`WUADVI_SPLASH_MS`, default 4 s) has been served.
4. When the ESP32 sends `DISPLAY_START` (only honored in `READY`), the control
   link is torn down and the same SPI pins are re-armed as the **fixed-size
   rect DMA receiver**.
5. From then on every valid rect packet is blitted into the PicoDVI
   framebuffer — the ESP32 fully owns the screen. The watchdog (8 s) resets
   the RP if the loop ever stalls.

If no ESP32 speaks, the splash simply stays on screen answering pings — that
is the "no application processor" idle state.

The companion ESP32 firmware lives in the **WuaDVI-firmware** repository: it
embeds the released `.uf2` of this project, probes the version over the
control link, re-flashes the RP via ROM UART boot (datasheet §5.8) when the
version or resolution differs, and then streams the LVGL UI.

---

## Control link protocol (v1)

Defined in [`include/link_protocol.h`](include/link_protocol.h) — a
byte-identical copy lives in the WuaDVI-firmware repo; the wire format is the
contract between both firmwares.

- Bus: SPI mode 1, MSB first, 1 MHz, same pins as the framebuffer stream.
- Master → slave: fixed 32-byte packets
  `A5 3C | type | arg | payload[27] | xor-checksum`.
  - `PING (0x01)` — slave preloads the PONG into its TX FIFO; master clocks it
    out ≥ 25 ms later.
  - `DISPLAY_START (0x03)` — slave switches to the rect receiver. The master
    must stay off the bus for ≥ 50 ms after sending it.
- Slave → master PONG (8 bytes):
  `3C A5 | ver_major | ver_minor | ver_patch | mode | res_id | xor-checksum`.
- Modes: `1 = SPLASH`, `2 = READY`, `3 = DISPLAY` (reserved). Resolution ids:
  `1 = 320x240`, `2 = 400x240`, `3 = 640x480x1`, `4 = 800x600x1`.

## Framebuffer rect protocol

Defined in [`include/frame_config.h`](include/frame_config.h). Every SPI
transaction is exactly `RECT_TOTAL_SIZE` bytes inside one CS-low envelope:

```
Offset  Size  Field
------  ----  -----
   0      4   Magic 0xA5 0xB6 0xC7 0xE9
   4      2   x1 (uint16 LE)      inclusive rect bounds,
   6      2   y1 (uint16 LE)      top-left origin
   8      2   x2 (uint16 LE)
  10      2   y2 (uint16 LE)
  12      N   pixel payload, zero-padded to RECT_PAYLOAD_MAX
```

- RGB565 modes: payload is `rect_w × rect_h × 2` bytes, row-major, RGB565 LE.
- Mono modes: payload is 1 bit/pixel, MSB first, each row padded to a byte.
- The fixed total size lets the slave use a fixed-length DMA transfer; the
  zero padding costs wire time but removes any per-packet re-configuration.
- The receiver recovers from the PL022 1-bit slave skew (packet arrives
  left-shifted by one bit) and re-synchronizes after any bad-magic packet by
  swallowing bytes until the bus is quiet for 300 µs (the master's ≥ 4 ms
  inter-packet gap guarantees a quiet window only exists at packet
  boundaries).

### Timing

| Parameter | Value |
|---|---|
| SPI clock | 25 MHz (master-driven) |
| Rect packet (24 lines full width) | ≈ 0.7–5 ms depending on mode |
| Inter-packet gap required | ≥ 4 ms (blit + DMA re-arm; 8-byte RX FIFO) |
| DVI output | 60 Hz, independent (PicoDVI DMA on core 1) |

---

## Resolution

The native framebuffer resolution is selectable at **compile time** via one of four PlatformIO environments in [`platformio.ini`](platformio.ini) — `rp2350_pizero_320x240`, `rp2350_pizero_400x240`, `rp2350_pizero_640x480x1`, `rp2350_pizero_800x600x1` — each setting a different `-DWUADVI_RES_*` build flag. The framebuffer lives uncompressed in the RP2350's 520 KB SRAM, so its size is the limiting factor. Four modes are available:

| Build flag               | Native FB | Framebuffer | Color              | DVI output        | Sysclock / VREG   |
|--------------------------|-----------|-------------|--------------------|-------------------|-------------------|
| `-DWUADVI_RES_320x240`   | 320 × 240 | 150 KB      | RGB565 (16-bit)    | 640 × 480p60      | 252 MHz / 1.20 V  |
| `-DWUADVI_RES_400x240`   | 400 × 240 | 192 KB      | RGB565 (16-bit)    | 800 × 480p60      | 295 MHz / 1.20 V  |
| `-DWUADVI_RES_640x480x1` | 640 × 480 | 37.5 KB     | Monochrome (1-bit) | 640 × 480p60      | 252 MHz / 1.20 V  |
| `-DWUADVI_RES_800x600x1` | 800 × 600 | 58.6 KB     | Monochrome (1-bit) | 800 × 600p60 (RB) | 354 MHz / 1.30 V  |

- **RGB565 modes (320×240 / 400×240)** — full color via `DVIGFX16` with 2× pixel doubling (square pixels). This is the original, hardware-verified pipeline.
- **Monochrome mode (640×480x1)** — `DVIGFX1`, 1 bit/pixel, native (1:1) pixels. This is the only way to reach a true 640×480 on this board: full-color 640×480 RGB565 needs 600 KB and exceeds the 520 KB SRAM, and an 8-bit indexed alternative isn't viable (the real-time TMDS encoder can't sustain 640 unique pixels per scanline). The 1-bit TMDS encoder is cheap enough to drive native 640×480.
- **Monochrome mode (800×600x1)** — same `DVIGFX1` pipeline at native 800×600 using the **CVT reduced-blanking** timing (RB) added to our PicoDVI fork (`DVI_RES_800x600p60_reduced`). Two caveats:
  - **Hardest overclock of the set**: the sysclock runs at the TMDS bit clock, 354 MHz @ 1.30 V (stock RP2350 rating is 150 MHz). Stability is chip-lot dependent — validate on your board. `main.cpp` raises the QMI flash clock divider to 4 before `begin()` so XIP stays within the QSPI flash's rated speed.
  - **Monitor compatibility**: some older monitors reject reduced-blanking 800×600. The full VESA-blanking variant (`DVI_RES_800x600p60`, 400 MHz) is also exposed in the fork if a monitor refuses the RB signal and your chip tolerates the higher clock.

> **⚠️ Monochrome flat-field banding.** In 1-bit mode, large flat areas (e.g. a solid white background) show faint, regularly-spaced vertical lines — an artifact inherent to TMDS-encoding a constant 1-bit field, not a firmware bug. Busy content and **dark backgrounds mask it completely**, so prefer black/dark backgrounds with light foreground shapes in this mode (the boot splash follows this rule). For artifact-free flat fields, use an RGB565 mode instead.

All modes except 800×600x1 share `VREG_VOLTAGE_1_20`; 800×600x1 uses `VREG_VOLTAGE_1_30`, passed explicitly to the `DVIGFX1` constructor via `WUADVI_VREG` in [`include/dvi_config.h`](include/dvi_config.h) (the voltage column in the library's `dvispec` table is dead code — `begin()` applies the constructor parameter).

> **Both firmwares must select the same flag/resolution.** The ESP32 sends dirty-rect coordinates relative to `SCREEN_W`/`SCREEN_H`; if the two sides disagree, rects won't line up with the RP2350's framebuffer. The PONG's resolution id exists precisely so the ESP32 can detect the mismatch and re-flash the matching UF2.

---

## Versioning & releases

The firmware version lives in [`include/version.h`](include/version.h)
(`WUADVI_FW_VERSION_*`) and is reported both on the splash and in every PONG.

Release procedure:

1. Bump `WUADVI_FW_VERSION_*` in `include/version.h`.
2. Merge to `main` and push a matching tag, e.g. `v1.0.0`.
3. The release workflow builds all four environments and attaches one UF2 per
   resolution (`WuaDVI-rp-lite-v1.0.0-<resolution>.uf2`) to the GitHub
   release.
4. The WuaDVI-firmware repo (ESP32) vendors those UF2s; on boot the ESP32
   compares the PONG version/resolution against its embedded payload and
   re-flashes the RP through ROM UART boot when they differ.

---

## Hardware

### Board

| Field  | Value |
|--------|-------|
| MCU    | RP2354B (WuaDVI board) — development also runs on Waveshare RP2350-PiZero |
| Flash  | 2 MB in-package (RP2354B) |

### SPI Pins (Slave — SPI1, shared by control link and rect stream)

| Signal | GPIO | Direction     | Description              |
|--------|------|---------------|--------------------------|
| SCK    | 10   | Input         | Clock driven by ESP32    |
| RX     | 12   | Input (MOSI)  | Commands / pixel data    |
| TX     | 11   | Output (MISO) | PONG responses           |
| CS     | 13   | Input         | Active low, packet envelope |

### DVI / HDMI Pins (GPIO bank 32-39)

| Signal   | GPIO   | Description     |
|----------|--------|-----------------|
| TMDS0+/− | 36/37  | Data lane 0     |
| TMDS1+/− | 34/35  | Data lane 1     |
| TMDS2+/− | 32/33  | Data lane 2     |
| CLK+/−   | 38/39  | Pixel clock     |

> **Note:** GPIOs 32-39 are above the default PIO window (0-31). `pio_set_gpio_base(pio0, 16)` is called in `setup()` to shift the window to 16-47 before PicoDVI initializes.

---

## Project Structure

```
WuaDVI-rp-lite/
├── platformio.ini          One build environment per resolution
├── include/
│   ├── version.h           Firmware version + product identity strings
│   ├── dvi_config.h        PicoDVI pin mapping, resolution defines, WuaDVI class
│   ├── link_protocol.h     Control link wire format (shared with WuaDVI-firmware)
│   ├── link_slave.h        Control link slave API
│   ├── frame_config.h      Rect protocol constants (magic, sizes)
│   ├── splash.h            Boot splash API
│   ├── spi_slave.h         Rect receiver API and SPI pin defines
│   └── serial_test.h       USB-serial test harness (SERIAL_TEST builds)
├── src/
│   ├── main.cpp            Boot state machine: splash → ready → display
│   ├── link_slave.cpp      Polled SPI slave for the control link
│   ├── splash.cpp          Splash rendering (RGB565 + mono, all resolutions)
│   ├── spi_slave.cpp       DMA-driven fixed-size rect receiver
│   └── serial_test.cpp     USB-serial frame test (SERIAL_TEST builds)
└── test/
    └── send_frame.py       PC-side test pattern sender (SERIAL_TEST builds)
```

---

## Build & Flash

There is one PlatformIO environment per resolution — pick the one matching the
ESP32 firmware's active `-DWUADVI_RES_*` flag:

```bash
# Build one resolution
pio run -e rp2350_pizero_640x480x1

# Upload (UF2 drag-and-drop or picotool)
pio run -e rp2350_pizero_640x480x1 --target upload

# Monitor (115200)
pio device monitor
```

Running `pio run` with no `-e` builds all four environments — useful to
confirm every resolution still compiles.

For a bare board, hold **BOOTSEL** while plugging USB to enter the UF2
bootloader. On the WuaDVI board the normal path is different: the ESP32
flashes the RP automatically through the ROM UART boot — see the
WuaDVI-firmware repository.

### USB-serial test mode

Uncomment `-DSERIAL_TEST` in `platformio.ini` to replace the SPI pipeline with
a USB-CDC frame receiver (no splash, no control link) and use
`test/send_frame.py` to push test patterns from a PC.

---

## Dependencies

| Library       | Source                               | Purpose            |
|---------------|--------------------------------------|--------------------|
| PicoDVI (fork)| `https://github.com/wualink/PicoDVI` | DVI/HDMI output    |
| pico-sdk      | Bundled with earlephilhower core     | DMA, SPI, PIO APIs |
