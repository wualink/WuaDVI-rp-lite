# WuaDVI-rp-lite

RP2350-PiZero firmware that acts as a **dumb DVI framebuffer renderer**.  
It receives a raw RGB565 framebuffer over SPI from an ESP32 running LVGL, and streams it to a monitor via HDMI/DVI using the PicoDVI library.

No UI logic lives here. The RP2350 knows nothing about widgets, layouts, or events — it only copies pixels to a screen.

---

## System Architecture

```
┌─────────────────────────────┐        SPI (20 MHz)        ┌──────────────────────────────┐
│       ESP32  (Master)       │ ─────────────────────────► │      RP2350-PiZero (Slave)   │
│                             │                             │                              │
│  LVGL renders UI            │   153 604 bytes / frame    │  Receives framebuffer via    │
│  into RGB565 framebuffer    │   [4B header + 153 600B]   │  DMA → copies to PicoDVI     │
│  → sends over SPI           │                             │  buffer → outputs DVI signal │
└─────────────────────────────┘                             └──────────┬───────────────────┘
                                                                       │ HDMI
                                                                       ▼
                                                                   Monitor
                                                            640×480 or 800×480 @ 60 Hz
                                                       (2× pixel-doubled from 320×240 / 400×240)
```

---

## Resolution

The native framebuffer resolution is selectable at **compile time** via a build flag in [`platformio.ini`](platformio.ini). The framebuffer lives uncompressed in the RP2350's 520 KB SRAM, so its size is the limiting factor. Three modes are available:

| Build flag               | Native FB | Framebuffer | Color              | DVI output   | VREG   |
|--------------------------|-----------|-------------|--------------------|--------------|--------|
| `-DWUADVI_RES_320x240`   | 320 × 240 | 150 KB      | RGB565 (16-bit)    | 640 × 480p60 | 1.20 V |
| `-DWUADVI_RES_400x240`   | 400 × 240 | 192 KB      | RGB565 (16-bit)    | 800 × 480p60 | 1.20 V |
| `-DWUADVI_RES_640x480x1` | 640 × 480 | 37.5 KB     | Monochrome (1-bit) | 640 × 480p60 | 1.20 V |

- **RGB565 modes (320×240 / 400×240)** — full color via `DVIGFX16` with 2× pixel doubling (square pixels). This is the original, hardware-verified pipeline.
- **Monochrome mode (640×480x1)** — `DVIGFX1`, 1 bit/pixel, native (1:1) pixels. This is the only way to reach a true 640×480 on this board: full-color 640×480 RGB565 needs 600 KB and exceeds the 520 KB SRAM, and an 8-bit indexed alternative isn't viable on the RP2350-PiZero (its real-time TMDS encoder can't sustain 640 unique pixels per scanline). The 1-bit TMDS encoder is cheap enough to drive native 640×480.

> **⚠️ Monochrome flat-field banding.** In 1-bit mode, large flat areas (e.g. a solid white background) show faint, regularly-spaced vertical lines — an artifact inherent to TMDS-encoding a constant 1-bit field, not a firmware bug. Busy content and **dark backgrounds mask it completely**, so prefer black/dark backgrounds with light foreground shapes in this mode. For artifact-free flat fields, use an RGB565 mode instead.

All modes share `VREG_VOLTAGE_1_20`.

```ini
build_flags =
    ; Uncomment exactly ONE — must match the flag in WuaDVI-esp32-lvgl:
    -DWUADVI_RES_320x240
    ; -DWUADVI_RES_400x240
    ; -DWUADVI_RES_640x480x1
```

> **Both firmwares must select the same flag.** The ESP32 sends dirty-rect coordinates relative to `SCREEN_W`/`SCREEN_H`; if the two sides disagree, rects won't line up with the RP2350's framebuffer (and out-of-bounds rects are silently dropped).

Selecting a flag sets `DVI_RESOLUTION`, `SCREEN_W` and `SCREEN_H` in [`include/dvi_config.h`](include/dvi_config.h); if no flag is defined the build fails with a `#error`.

---

## Hardware

### Board

| Field  | Value |
|--------|-------|
| MCU    | RP2350B |
| Board  | Waveshare RP2350-PiZero |
| Flash  | 16 MB |

### SPI Pins (Slave — SPI1)

| Signal | GPIO | Direction     | Description              |
|--------|------|---------------|--------------------------|
| SCK    | 10   | Input         | Clock driven by ESP32    |
| RX     | 12   | Input (MOSI)  | Framebuffer data from ESP32 |
| TX     | 11   | Output (MISO) | Sends zeros (unused)     |
| CS     | 13   | Input         | Active low, frame boundary |

### DVI / HDMI Pins (RP2350B GPIO bank 32-39)

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
├── platformio.ini          Build configuration (PicoDVI only, no LVGL)
├── include/
│   ├── dvi_config.h        PicoDVI pin mapping and resolution defines
│   ├── frame_config.h      SPI frame protocol constants (magic, sizes)
│   └── spi_slave.h         SPI slave API and pin defines
└── src/
    ├── main.cpp            Arduino setup/loop: DVI init, SPI polling, memcpy
    └── spi_slave.cpp       DMA-driven SPI1 slave driver
```

### `include/dvi_config.h`

Defines the `dvi_serialiser_cfg` struct with the physical GPIO assignments for the Waveshare RP2350-PiZero board, plus a `#if` block that maps the resolution build flag (see [Resolution](#resolution)) to the matching `DVI_RESOLUTION` macro and screen dimension constants (`SCREEN_W`, `SCREEN_H`).

### `include/frame_config.h`

Single source of truth for the SPI framebuffer protocol:

- Magic header bytes (`0xA5 0xB6 0xC7 0xD8`)
- `FRAME_HEADER_SIZE` — 4 bytes
- `FRAME_PIXEL_BYTES` — `320 × 240 × 2 = 153 600` bytes
- `FRAME_TOTAL_SIZE`  — `153 604` bytes (header + pixels)

Both the RP2350 and the ESP32 must agree on these values.

### `include/spi_slave.h`

Declares the SPI slave API:

- `spi_slave_init()` — configures SPI1 and starts both DMA channels.
- `spi_slave_poll()` — returns `true` when a valid frame has been received and re-arms the DMA for the next one.
- `spi_pixels` — `const uint8_t*` pointing to the pixel region of the last received frame (valid until the next `spi_slave_poll()` that returns `true`).

### `src/spi_slave.cpp`

Implements a zero-copy, DMA-driven SPI slave:

- Two DMA channels: **RX** (SPI FIFO → RAM, 153 604 bytes) and **TX** (RAM → SPI FIFO, sends zeros).
- `spi_slave_poll()` checks `dma_channel_is_busy()`. When the channel finishes, it validates the 4-byte magic header, re-arms both channels, and returns the validity result.
- Because a full frame takes ~61 ms to arrive at 20 MHz, there is ample time to `memcpy` the pixels to the DVI buffer before the next frame overwrites the RX buffer.

### `src/main.cpp`

Arduino entry points:

- **`setup()`** — Calls `pio_set_gpio_base()`, initializes PicoDVI (`dvi_display.begin()`), fills the screen blue (waiting indicator), then calls `spi_slave_init()` and enables the hardware watchdog (8 s timeout).
- **`loop()`** — Polls `spi_slave_poll()`; on a valid frame, calls `memcpy` from `spi_pixels` into `dvi_display.getBuffer()`. Logs frame counts every 5 s and a heartbeat every 10 s via the non-blocking `safe_printf` wrapper.

---

## SPI Frame Protocol

Every SPI transaction consists of exactly one CS assertion containing `FRAME_TOTAL_SIZE = 153 604` bytes:

```
Byte offset    Size      Field
───────────   ──────    ──────────────────────────────────────────────
0              1        Magic byte 0  (0xA5)
1              1        Magic byte 1  (0xB6)
2              1        Magic byte 2  (0xC7)
3              1        Magic byte 3  (0xD8)
4        153 600        RGB565 pixel data, row-major, top-left origin
```

### Pixel format

| Property     | Value                              |
|--------------|------------------------------------|
| Color depth  | 16-bit RGB565                      |
| Byte order   | Same as LVGL `LV_COLOR_FORMAT_RGB565` |
| Layout       | Row-major, top-left origin         |
| Dimensions   | `SCREEN_W` × `SCREEN_H` (320×240 or 400×240, see [Resolution](#resolution)) |

The RP2350 copies the received bytes verbatim into the PicoDVI framebuffer. No color conversion is performed, so the ESP32 must produce pixels in the exact same format that PicoDVI expects.

### Timing

| Parameter           | Value                    |
|---------------------|--------------------------|
| SPI clock           | 20 MHz (master-driven)   |
| Transfer time       | ≈ 61 ms per frame        |
| Max throughput      | ≈ 16 fps                 |
| memcpy time (RP2350)| < 2 ms                   |
| DVI output rate     | 60 Hz (independent of SPI, driven by PicoDVI DMA on core1) |

> To reach ~32 fps, increase the ESP32 SPI master clock to 40 MHz and update `SPI_SPEED_HZ` in `spi_slave.cpp` accordingly.

---

## What the ESP32-LVGL Project Must Provide

This firmware is passive — it only renders what it receives. The companion **WuaDVI-esp32-lvgl** project is responsible for:

1. **Running LVGL** with an RGB565 display driver at the **same resolution flag** selected here (320×240 or 400×240, see [Resolution](#resolution)) that targets an in-memory framebuffer (not a real display).
2. **Sending complete frames** over SPI as described above:
   - Assert CS low.
   - Transmit 4 magic bytes (`0xA5 0xB6 0xC7 0xD8`).
   - Transmit 153 600 bytes of RGB565 pixel data from the LVGL framebuffer.
   - De-assert CS high.
3. **Matching the SPI bus configuration**:
   - Mode 1 (CPOL = 0, CPHA = 1)
   - MSB first
   - Clock: 20 MHz (must match `SPI_SPEED_HZ` in `spi_slave.cpp`)
4. **Controlling frame rate**: the ESP32 must not start a new transfer before the RP2350 has had time to process the previous one. At 20 MHz a full frame takes ~61 ms, so back-to-back transfers at any rate up to ~16 fps are safe without a handshake signal.
5. **Pixel format compatibility**: LVGL must be configured so that its internal color format matches `LV_COLOR_FORMAT_RGB565` with the same byte order as PicoDVI's framebuffer.

---

## Build & Flash

```bash
# Build
pio run -e rp2350_pizero

# Upload (UF2 drag-and-drop or picotool)
pio run -e rp2350_pizero --target upload

# Monitor
pio device monitor
```

Hold **BOOTSEL** while plugging the USB cable to enter UF2 bootloader mode for the first flash.

---

## Boot Sequence

1. Serial initialized (3 s delay for USB-CDC enumeration).
2. `pio_set_gpio_base(pio0, 16)` — extends PIO window to reach GPIOs 32-47.
3. PicoDVI starts; screen turns **blue** (waiting for ESP32).
4. SPI1 slave + DMA channels armed.
5. Watchdog enabled (8 s reset if loop stalls).
6. Main loop: polls SPI, copies frames to DVI buffer on each valid reception.

---

## Dependencies

| Library       | Source                              | Purpose            |
|---------------|-------------------------------------|--------------------|
| PicoDVI (fork)| `https://github.com/Caza20/PicoDVI` | DVI/HDMI output    |
| pico-sdk      | Bundled with earlephilhower core    | DMA, SPI, PIO APIs |
# WuaDVI-rp-lite
