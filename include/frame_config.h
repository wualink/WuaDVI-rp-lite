#pragma once
#include <stdint.h>
#include "dvi_config.h"

/*
 * SPI framebuffer protocol — WuaDVI-rp-lite / WuaDVI-esp32-lvgl
 *
 * Two protocols share this file:
 *
 *   1) RECT_* (partial-rect protocol) — used over SPI from the ESP32 master.
 *      Each SPI transaction (one CS assertion) carries exactly RECT_TOTAL_SIZE
 *      bytes: a 12-byte header (4-byte magic + 4 × uint16 LE coords) followed
 *      by RECT_PAYLOAD_MAX bytes of pixel data.  Only (x2-x1+1)*(y2-y1+1)*2
 *      of those pixel bytes are valid; the rest are zero padding so the DMA
 *      RX channel can use a fixed-length transfer.
 *
 *        Offset  Size  Field
 *        ------  ----  -----
 *           0      4   RECT_MAGIC_0..3  (0xA5 0xB6 0xC7 0xE9)
 *           4      2   x1               (uint16 LE)
 *           6      2   y1               (uint16 LE)
 *           8      2   x2               (uint16 LE)
 *          10      2   y2               (uint16 LE)
 *          12   …N     pixel payload (row-major within the rect, LE RGB565),
 *                      zero-padded to RECT_PAYLOAD_MAX.
 *
 *   2) FRAME_* (full-frame protocol) — kept only for the USB-serial test path
 *      (serial_test.cpp + test/send_frame.py).  One transaction =
 *      4-byte magic + 153 600 bytes of pixels.  No longer used over SPI.
 */

/* ── Full-frame protocol (legacy, USB-serial test only) ─────────────────── */
#define FRAME_MAGIC_0  0xA5u
#define FRAME_MAGIC_1  0xB6u
#define FRAME_MAGIC_2  0xC7u
#define FRAME_MAGIC_3  0xD8u

#define FRAME_HEADER_SIZE   4u
#define FRAME_PIXEL_BYTES   ((uint32_t)(SCREEN_W) * (SCREEN_H) * 2u)  /* 150 KB @320x240 */
#define FRAME_TOTAL_SIZE    (FRAME_HEADER_SIZE + FRAME_PIXEL_BYTES)

/* ── Rect protocol (SPI from ESP32) ─────────────────────────────────────── */
#define RECT_MAGIC_0  0xA5u
#define RECT_MAGIC_1  0xB6u
#define RECT_MAGIC_2  0xC7u
#define RECT_MAGIC_3  0xE9u   /* differs from FRAME_MAGIC_3 = 0xD8 */

/* Header: 4 B magic + 4 × uint16 coords = 12 bytes. */
#define RECT_HEADER_SIZE    12u

/*
 * Must match the ESP32 side exactly.  Sized so a full-width strip of
 * PARTIAL_BUF_LINES (24) lines fits in one packet.
 */
#define PARTIAL_BUF_LINES   24u
#define RECT_PAYLOAD_MAX    ((uint32_t)(SCREEN_W) * (PARTIAL_BUF_LINES) * 2u)  /* 15 360 @320w */
#define RECT_TOTAL_SIZE     ((uint32_t)(RECT_HEADER_SIZE) + (RECT_PAYLOAD_MAX))
