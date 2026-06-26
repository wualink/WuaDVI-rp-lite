#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "frame_config.h"

/*
 * Serial (USB-CDC) framebuffer test mode.
 *
 * Enabled by compiling with -DSERIAL_TEST (see platformio.ini).
 * Replaces SPI reception: the PC sends the same frame protocol over USB Serial
 * and the RP2350 displays it via DVI — no ESP32 required.
 *
 * Frame format (identical to the SPI protocol in frame_config.h):
 *   4 bytes            magic  (0xA5 0xB6 0xC7 0xD8)
 *   FRAME_PIXEL_BYTES  pixel data, row-major, top-left origin.  The pixel
 *                      width follows the selected mode (WUADVI_PIXEL_BYTES):
 *                      RGB565 (2 B/px in 320x240 / 400x240).
 *
 * Use the companion Python script  test/send_frame.py  to send frames.
 */

/* Call once in setup(), after Serial.begin() has already been called. */
void serial_test_init(void);

/*
 * Non-blocking poll.  Returns true when a complete, valid frame has been
 * received and written directly into dvi_fb (FRAME_PIXEL_BYTES bytes).
 * Returns false when:
 *   - fewer than 4 header bytes are available yet (no frame started), OR
 *   - the 4-byte magic does not match (bad sync; frame discarded), OR
 *   - the pixel data timed out before FRAME_PIXEL_BYTES were received.
 *
 * dvi_fb must point to the PicoDVI framebuffer (dvi_display.getBuffer()),
 * passed as raw bytes so it works regardless of the selected pixel mode.
 * Pixel data is written directly into it — no intermediate copy needed.
 */
bool serial_test_poll(uint8_t *dvi_fb);
