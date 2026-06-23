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
 * Uncomment exactly ONE of  -DWUADVI_RES_320x240 / -DWUADVI_RES_400x240.
 *
 * Both modes use full RGB565 color (DVIGFX16) with 2x pixel doubling.  The
 * framebuffer is stored uncompressed in the RP2350's 520 KB SRAM, so the
 * 16-bit framebuffer size is the limiting factor:
 *
 *   320x240 RGB565 = 150 KB  →  scanned out as 640x480p60
 *   400x240 RGB565 = 192 KB  →  scanned out as 800x480p60
 *
 * Both share VREG_VOLTAGE_1_20, so 400x240 needs no extra overclock margin.
 * The ESP32 side (WuaDVI-esp32-lvgl) must select the SAME resolution flag.
 */
#if defined(WUADVI_RES_400x240)
  #define DVI_RESOLUTION  DVI_RES_400x240p60
  #define SCREEN_W        400
  #define SCREEN_H        240
#elif defined(WUADVI_RES_320x240)
  #define DVI_RESOLUTION  DVI_RES_320x240p60
  #define SCREEN_W        320
  #define SCREEN_H        240
#else
  #error "No resolution selected — define WUADVI_RES_320x240 or WUADVI_RES_400x240 in platformio.ini build_flags"
#endif
