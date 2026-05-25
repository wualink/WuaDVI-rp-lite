#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "frame_config.h"

/*
 * SPI1 pin assignments — same physical wiring as before.
 * Verified working on hardware; do not change without updating the ESP32 master.
 */
#define WUADVI_SPI_PIN_SCK  10u
#define WUADVI_SPI_PIN_TX   11u   /* MISO — RP2350 → ESP32 (unused in this role) */
#define WUADVI_SPI_PIN_RX   12u   /* MOSI — ESP32 → RP2350 (framebuffer data)    */
#define WUADVI_SPI_PIN_CS   13u

/* Configure SPI1 as slave (Mode 1, MSB first) and start DMA channels. */
void spi_slave_init(void);

/*
 * Poll for a completed rect packet.
 * Returns true when:
 *   - The DMA RX channel has finished a RECT_TOTAL_SIZE transfer, AND
 *   - The 4-byte rect magic is valid (0xA5 0xB6 0xC7 0xE9).
 * On true: the rect coords and pixel data are accessible via spi_slave_blit_rect();
 *          DMA is re-armed for the next packet.
 * On false: transfer still in progress, or magic invalid (packet skipped).
 *
 * NOTE: the rect data inside the internal RX buffer is only valid until the
 * NEXT spi_slave_poll() call that returns true.  Consume it (e.g. via the
 * blit helper) before polling again.
 */
bool spi_slave_poll(void);

/*
 * Blit the last received rect (the one whose poll returned true) into a full
 * 320*240 RGB565 framebuffer, copying row by row at the correct stride.
 *
 * Must be called between a successful spi_slave_poll() and the next one.
 * No-op if the last polled packet had out-of-range coords.
 */
void spi_slave_blit_rect(uint16_t *dvi_fb);

/*
 * Diagnostics — counters since boot.  Useful to tell apart:
 *   - "DMA is never completing"  (completions == 0; wiring/clock issue)
 *   - "DMA completes but magic is wrong"  (bad_magic > 0; sync drift / glitches)
 *   - "Some bytes flow but never enough"  (dma_bytes_remaining stays high)
 */
uint32_t spi_slave_completions(void);    /* total DMA completions seen */
uint32_t spi_slave_valid_packets(void);  /* completions whose magic matched (clean or unshifted) */
uint32_t spi_slave_bad_magic(void);      /* completions with neither clean nor shifted magic */
uint32_t spi_slave_unshifted(void);      /* completions recovered via 1-bit unshift */
uint32_t spi_slave_dma_remaining(void);  /* current DMA bytes-remaining (live) */

/* Returns pointer to the first 4 bytes of the most recent bad-magic buffer.
 * Stable until the next bad-magic event.  Useful for printing a hex dump. */
const uint8_t *spi_slave_last_bad_header(void);
