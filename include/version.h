/**
 * @file version.h
 * @brief Firmware version and product identity of the WuaDVI RP2354B firmware.
 *
 * Single source of truth for everything shown on the boot splash and reported
 * over the control link PONG (see link_protocol.h / link_slave.h).
 *
 * Release procedure: bump the three numbers below, merge to main and push the
 * matching tag (e.g. v1.0.0). The release workflow then publishes one .uf2
 * per resolution; the ESP32 firmware (WuaDVI-firmware repo) embeds those
 * assets and re-flashes any RP whose reported version differs.
 */
#pragma once

/* ── Firmware semantic version — MUST match the release tag ─────────────── */
#define WUADVI_FW_VERSION_MAJOR  1
#define WUADVI_FW_VERSION_MINOR  0
#define WUADVI_FW_VERSION_PATCH  0

/* Stringified "M.m.p" helper, usable directly in string literals. */
#define WUADVI_STR2(x)  #x
#define WUADVI_STR(x)   WUADVI_STR2(x)
#define WUADVI_FW_VERSION_STRING            \
    WUADVI_STR(WUADVI_FW_VERSION_MAJOR) "." \
    WUADVI_STR(WUADVI_FW_VERSION_MINOR) "." \
    WUADVI_STR(WUADVI_FW_VERSION_PATCH)

/* ── Product / organization identity (boot splash strings) ──────────────── */
#define WUADVI_ORG_NAME         "WUALINK"
#define WUADVI_ORG_TAGLINE      "member of Wualabs  -  wualabs.com"
#define WUADVI_PRODUCT_NAME     "WuaDVI v1.0"

/* ── Boot splash duration ────────────────────────────────────────────────
 * Minimum time the splash stays on screen before the firmware reports READY
 * on the control link and accepts a DISPLAY_START command.  Overridable from
 * platformio.ini build_flags (-DWUADVI_SPLASH_MS=...).                      */
#ifndef WUADVI_SPLASH_MS
#define WUADVI_SPLASH_MS  4000u
#endif
