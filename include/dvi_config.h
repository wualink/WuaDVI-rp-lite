#pragma once

#include <PicoDVI.h>

/*
 * Custom pin config for Waveshare RP2350-PiZero (RP2350B, GPIOs 32-39 for HDMI).
 * Identical to WuaDVI-rp-lvgl — verified working on hardware.
 *
 *   GPIO32/33 -> TMDS2 (D2+/D2-)
 *   GPIO34/35 -> TMDS1 (D1+/D1-)
 *   GPIO36/37 -> TMDS0 (D0+/D0-)
 *   GPIO38/39 -> CLK+/CLK-
 */
static const struct dvi_serialiser_cfg rp2350_pizero_cfg = {
  .pio        = pio0,
  .sm_tmds    = {0, 1, 2},
  .pins_tmds  = {36, 34, 32},
  .pins_clk   = 38,
  .invert_diffpairs = false
};

#define DVI_BOARD_CFG  rp2350_pizero_cfg

/*
 * Resolution select — chosen via build_flags in platformio.ini.
 * Uncomment exactly ONE of the WUADVI_RES_* flags.
 *
 * There are two distinct pixel pipelines, selected by the flag:
 *
 *   ── Full-color modes (DVIGFX16, RGB565, 2 bytes/pixel) ──────────────────
 *   These keep the original, hardware-verified pipeline UNCHANGED.  The
 *   framebuffer is pixel-doubled 2x on both axes, so square pixels are
 *   preserved (a circle stays a circle).  Limited by the 16-bit framebuffer
 *   size in the RP2350's 520 KB SRAM:
 *
 *     320x240 RGB565 = 150 KB  →  scanned out as 640x480p60  (pixel 2x2)
 *     400x240 RGB565 = 192 KB  →  scanned out as 800x480p60  (pixel 2x2)
 *
 *   ── Monochrome mode (DVIGFX1, 1 bit/pixel) ──────────────────────────────
 *   The 1-bit TMDS encoder is far cheaper, so it CAN drive native 640x480:
 *
 *     640x480 mono = 37.5 KB framebuffer  →  640x480p60 native (pixel 1x1)
 *
 *   Square pixels, native resolution, black & white only.  This is the library
 *   stock 1-bit example mode (DVIGFX1 @ DVI_RES_640x480p60).
 *
 * All modes share VREG_VOLTAGE_1_20.  The ESP32 side (WuaDVI-esp32-lvgl) MUST
 * select the SAME resolution flag — the wire format (bytes/pixel, coords)
 * derives from it on both ends; a mismatch scrambles the image.
 *
 * Derived macros used across the firmware:
 *   WUADVI_COLOR_MONO     defined only in 1-bit mode (selects DVIGFX1 path)
 *   WUADVI_PIXEL_BYTES    framebuffer/wire bytes per pixel (2 = RGB565)
 */
#if defined(WUADVI_RES_640x480x1)
  #define DVI_RESOLUTION        DVI_RES_640x480p60
  #define SCREEN_W              640
  #define SCREEN_H              480
  #define WUADVI_COLOR_MONO     1     /* DVIGFX1, 1 bit/pixel */
  #define WUADVI_PIXEL_BYTES    1u    /* placeholder; mono uses bit-packing, not byte/pixel */
#elif defined(WUADVI_RES_400x240)
  #define DVI_RESOLUTION        DVI_RES_400x240p60
  #define SCREEN_W              400
  #define SCREEN_H              240
  #define WUADVI_PIXEL_BYTES    2u    /* RGB565 */
#elif defined(WUADVI_RES_320x240)
  #define DVI_RESOLUTION        DVI_RES_320x240p60
  #define SCREEN_W              320
  #define SCREEN_H              240
  #define WUADVI_PIXEL_BYTES    2u    /* RGB565 */
#else
  #error "No resolution selected — define WUADVI_RES_320x240, WUADVI_RES_400x240 or WUADVI_RES_640x480x1 in platformio.ini build_flags"
#endif

/*
 * WuaDVI — the display class used by the sketch.  Construct it once and call
 * the usual Adafruit_GFX methods (begin(), fillScreen(), color565(),
 * getBuffer(), ...).  It wraps the right PicoDVI base for the selected mode so
 * main.cpp stays identical across resolutions:
 *
 *     WuaDVI dvi_display(DVI_RESOLUTION, DVI_BOARD_CFG);
 *     dvi_display.begin();
 *     dvi_display.fillScreen(dvi_display.color565(0, 0, 128));
 *
 * RGB565 modes (320x240 / 400x240): WuaDVI is just DVIGFX16 — behavior is
 * exactly the original, hardware-verified pipeline.
 *
 * Monochrome mode (640x480x1): WuaDVI is DVIGFX1.  color565()/fillScreen()
 * keep the same signatures but collapse color to 1 bit by luminance threshold,
 * and drawTestPattern() paints a static image used to validate that the native
 * 640x480 mono signal locks (the 1-bit SPI pipeline is not built yet).
 */
#if defined(WUADVI_COLOR_MONO)
class WuaDVI : public DVIGFX1 {
public:
    WuaDVI(const DVIresolution res, const struct dvi_serialiser_cfg &cfg)
        : DVIGFX1(res, false /* single-buffered */, cfg) {}

    /* Same helper DVIGFX16 exposes, so call sites build colors identically. */
    uint16_t color565(uint8_t red, uint8_t green, uint8_t blue) {
        return (uint16_t)(((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3));
    }

    /* fillScreen(RGB565): pick white (1) or black (0) by luminance. */
    void fillScreen(uint16_t color565) {
        DVIGFX1::fillScreen(lum_bit(color565));
    }

    /* Static signal-test image: white border, diagonals, a few filled boxes and
     * a label.  If this shows crisp and stable, native 640x480 mono works. */
    void drawTestPattern(void) {
        DVIGFX1::fillScreen(0);
        drawRect(0, 0, SCREEN_W, SCREEN_H, 1);
        drawRect(1, 1, SCREEN_W - 2, SCREEN_H - 2, 1);
        drawLine(0, 0, SCREEN_W - 1, SCREEN_H - 1, 1);
        drawLine(SCREEN_W - 1, 0, 0, SCREEN_H - 1, 1);
        for (int x = 0; x < SCREEN_W; x += 32) drawFastVLine(x, 0, SCREEN_H, 1);
        for (int y = 0; y < SCREEN_H; y += 32) drawFastHLine(0, y, SCREEN_W, 1);
        fillRect(40, 40, 120, 80, 1);
        setTextColor(1);
        setTextSize(2);
        setCursor(180, 70);
        print("WuaDVI 640x480 mono");
    }

private:
    static uint8_t lum_bit(uint16_t c) {
        const uint8_t r = (uint8_t)((c >> 8) & 0xF8);
        const uint8_t g = (uint8_t)((c >> 3) & 0xFC);
        const uint8_t b = (uint8_t)((c << 3) & 0xF8);
        return (uint8_t)(((r * 30 + g * 59 + b * 11) / 100) >= 128 ? 1 : 0);
    }
};
#else
class WuaDVI : public DVIGFX16 {
public:
    WuaDVI(const DVIresolution res, const struct dvi_serialiser_cfg &cfg)
        : DVIGFX16(res, cfg) {}
};
#endif
