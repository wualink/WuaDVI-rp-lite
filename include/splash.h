/**
 * @file splash.h
 * @brief WuaDVI boot splash screen (organization, product, version,
 *        resolution, animated "Loading" indicator).
 *
 * The splash is the first thing shown after DVI sync is established and it
 * stays on screen until the ESP32 takes over the display with the
 * framebuffer-rect stream.  The layout is computed from SCREEN_W/SCREEN_H
 * (percent-based positions, width-based text scale), so the same code renders
 * correctly at every supported resolution, in RGB565 as well as in 1-bit
 * monochrome.
 */
#pragma once
#include "dvi_config.h"

/**
 * @brief Draw the full boot splash on the given display.
 *
 * Renders the static content once: framed border, organization name and
 * tagline, product name, firmware version, active resolution and the
 * "Loading" caption.  Call a single time right after WuaDVI::begin().
 *
 * @param display  Active WuaDVI display (RGB565 or monochrome pipeline).
 */
void splash_draw(WuaDVI &display);

/**
 * @brief Animate the "Loading" ellipsis (one to three trailing dots).
 *
 * Rate-limited internally (~400 ms per step) — call from loop() as often as
 * convenient while the splash is on screen.  Safe to stop calling at any
 * time; the last drawn frame simply remains.
 *
 * @param display  Same display that was passed to splash_draw().
 */
void splash_tick(WuaDVI &display);
