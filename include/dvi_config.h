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
 * 320x240 native framebuffer → 640x480p60 DVI with 2x pixel doubling.
 * All monitors accept this timing.
 */
#define DVI_RESOLUTION  DVI_RES_320x240p60
#define SCREEN_W        320
#define SCREEN_H        240
