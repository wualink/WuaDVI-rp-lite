/**
 * @file splash.cpp
 * @brief WuaDVI boot splash screen implementation — see splash.h.
 */
#include "splash.h"
#include "version.h"
#include <Arduino.h>
#include <string.h>

/* ── Palette ──────────────────────────────────────────────────────────────
 * Monochrome pipeline: DVIGFX1 treats any nonzero color as a lit pixel, so
 * the palette collapses to 0/1.  A dark background is also deliberate there:
 * it masks the flat-field banding inherent to 1-bit TMDS (see README).
 * RGB565 pipeline: brand-ish colors, all bright enough to stay white if the
 * same UI were luminance-thresholded (keeps both pipelines consistent).    */
#if defined(WUADVI_COLOR_MONO)
  #define COL_BG      0u
  #define COL_FG      1u
  #define COL_ACCENT  1u
  #define COL_DIM     1u
#else
  /** Compile-time RGB888 → RGB565 conversion. */
  #define RGB565(r, g, b) \
      (uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3))
  #define COL_BG      RGB565(0, 0, 0)         /* black                     */
  #define COL_FG      RGB565(255, 255, 255)   /* white                     */
  #define COL_ACCENT  RGB565(255, 184, 0)     /* Wualabs amber             */
  #define COL_DIM     RGB565(168, 168, 168)   /* secondary gray            */
#endif

/* ── Layout ───────────────────────────────────────────────────────────────
 * One text-scale unit for the whole screen: 1 at 320/400 wide, 2 at 640/800.
 * Vertical anchors are percentages of SCREEN_H, so every resolution keeps
 * the same visual balance.  The classic GFX font cell is 6x8 px per scale
 * unit (5x7 glyph + 1 px spacing).                                          */
#define SPLASH_UNIT       ((SCREEN_W >= 640) ? 2 : 1)
#define FONT_W(size)      (6 * (size))
#define FONT_H(size)      (8 * (size))

/** Active-resolution caption shown under the firmware version. */
#if defined(WUADVI_RES_800x600x1)
  #define SPLASH_RES_TEXT  "800x600 mono - native 60 Hz"
#elif defined(WUADVI_RES_640x480x1)
  #define SPLASH_RES_TEXT  "640x480 mono - native 60 Hz"
#elif defined(WUADVI_RES_400x240)
  #define SPLASH_RES_TEXT  "400x240 RGB565 - 800x480p60 out"
#elif defined(WUADVI_RES_320x240)
  #define SPLASH_RES_TEXT  "320x240 RGB565 - 640x480p60 out"
#endif

#define LOADING_TEXT      "Loading"
#define LOADING_MAX_DOTS  3
#define LOADING_STEP_MS   400u

/* Ellipsis animation state, initialized by splash_draw(). */
static int16_t  s_dots_x     = 0;   /* left edge of the dots area           */
static int16_t  s_dots_y     = 0;
static uint8_t  s_dots_size  = 1;
static uint8_t  s_dots_count = 0;
static uint32_t s_last_step_ms = 0;

/**
 * @brief Print one line of text horizontally centered at a given baseline.
 * @param display  Target display.
 * @param y        Top edge of the text row, in pixels.
 * @param size     GFX text scale factor.
 * @param color    Text color (palette value).
 * @param text     NUL-terminated ASCII string.
 * @return X coordinate where the text starts (useful to anchor follow-ups).
 */
static int16_t draw_centered(WuaDVI &display, int16_t y, uint8_t size,
                             uint16_t color, const char *text) {
    const int16_t w = (int16_t)(strlen(text) * FONT_W(size));
    int16_t x = (int16_t)((SCREEN_W - w) / 2);
    if (x < 0) x = 0;
    display.setTextSize(size);
    display.setTextColor(color);
    display.setCursor(x, y);
    display.print(text);
    return x;
}

void splash_draw(WuaDVI &display) {
    const uint8_t u = (uint8_t)SPLASH_UNIT;

    display.fillScreen(COL_BG);

    /* Double frame border. */
    display.drawRect(0, 0, SCREEN_W, SCREEN_H, COL_FG);
    display.drawRect(3, 3, SCREEN_W - 6, SCREEN_H - 6, COL_FG);

    /* Organization block. */
    int16_t y = (int16_t)(SCREEN_H * 12 / 100);
    draw_centered(display, y, (uint8_t)(3 * u), COL_FG, WUADVI_ORG_NAME);
    y = (int16_t)(y + FONT_H(3 * u) + 6 * u);
    draw_centered(display, y, u, COL_DIM, WUADVI_ORG_TAGLINE);

    /* Separator rule. */
    y = (int16_t)(SCREEN_H * 32 / 100);
    display.drawFastHLine((int16_t)(SCREEN_W / 6), y,
                          (int16_t)(SCREEN_W * 2 / 3), COL_DIM);

    /* Product block. */
    y = (int16_t)(SCREEN_H * 40 / 100);
    draw_centered(display, y, (uint8_t)(2 * u), COL_ACCENT, WUADVI_PRODUCT_NAME);
    y = (int16_t)(y + FONT_H(2 * u) + 6 * u);
    draw_centered(display, y, u, COL_FG, "RP Firmware v" WUADVI_FW_VERSION_STRING);
    y = (int16_t)(y + FONT_H(u) + 4 * u);
    draw_centered(display, y, u, COL_FG, SPLASH_RES_TEXT);

    /* Loading caption — the trailing dots are animated by splash_tick().
     * Center the full "Loading..." footprint so the dots don't unbalance it. */
    const uint8_t load_size = (uint8_t)(2 * u);
    const int16_t load_y    = (int16_t)(SCREEN_H * 78 / 100);
    const int16_t full_w    = (int16_t)((strlen(LOADING_TEXT) + LOADING_MAX_DOTS)
                                        * FONT_W(load_size));
    int16_t load_x = (int16_t)((SCREEN_W - full_w) / 2);
    if (load_x < 0) load_x = 0;

    display.setTextSize(load_size);
    display.setTextColor(COL_FG);
    display.setCursor(load_x, load_y);
    display.print(LOADING_TEXT);

    s_dots_x      = (int16_t)(load_x + strlen(LOADING_TEXT) * FONT_W(load_size));
    s_dots_y      = load_y;
    s_dots_size   = load_size;
    s_dots_count  = 0;
    s_last_step_ms = millis();
}

void splash_tick(WuaDVI &display) {
    const uint32_t now = millis();
    if (now - s_last_step_ms < LOADING_STEP_MS) return;
    s_last_step_ms = now;

    s_dots_count = (uint8_t)((s_dots_count % LOADING_MAX_DOTS) + 1);

    /* Clear the dots area, then print the current number of dots. */
    display.fillRect(s_dots_x, s_dots_y,
                     (int16_t)(LOADING_MAX_DOTS * FONT_W(s_dots_size)),
                     FONT_H(s_dots_size), COL_BG);
    display.setTextSize(s_dots_size);
    display.setTextColor(COL_FG);
    display.setCursor(s_dots_x, s_dots_y);
    for (uint8_t i = 0; i < s_dots_count; ++i)
        display.print('.');
}
