#ifdef SERIAL_TEST

#include "serial_test.h"
#include "frame_config.h"
#include "hardware/watchdog.h"
#include <Arduino.h>

/* USB-CDC read timeout: if the Python script stalls mid-transfer, give up
 * after this many ms and return false so the watchdog can keep running. */
#define SERIAL_READ_TIMEOUT_MS  3000

void serial_test_init(void) {
    Serial.setTimeout(SERIAL_READ_TIMEOUT_MS);
}

bool serial_test_poll(uint16_t *dvi_fb) {
    /* Wait until at least the 4-byte magic header arrives. */
    if (Serial.available() < (int)FRAME_HEADER_SIZE) return false;

    uint8_t hdr[FRAME_HEADER_SIZE];
    if ((size_t)Serial.readBytes((char *)hdr, FRAME_HEADER_SIZE) != FRAME_HEADER_SIZE)
        return false;

    if (hdr[0] != FRAME_MAGIC_0 || hdr[1] != FRAME_MAGIC_1 ||
        hdr[2] != FRAME_MAGIC_2 || hdr[3] != FRAME_MAGIC_3) {
        /* Bad sync — caller will retry on the next loop iteration. */
        return false;
    }

    /* Read pixel data directly into the PicoDVI framebuffer in chunks.
     * Writing directly avoids an extra 150 KB intermediate buffer.
     * PicoDVI's DMA scanner on core1 reads the same buffer concurrently,
     * so there will be a brief transfer-time tear — acceptable in test mode. */
    uint8_t  *dest     = (uint8_t *)dvi_fb;
    uint32_t  received = 0;

    while (received < FRAME_PIXEL_BYTES) {
        /* Keep the watchdog alive during the transfer (~150 ms at USB speed). */
        watchdog_update();

        size_t got = Serial.readBytes(
            (char *)(dest + received),
            FRAME_PIXEL_BYTES - received);

        if (got == 0) break; /* timeout */
        received += (uint32_t)got;
    }

    return (received == FRAME_PIXEL_BYTES);
}

#endif /* SERIAL_TEST */
