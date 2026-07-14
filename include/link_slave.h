/**
 * @file link_slave.h
 * @brief Polled SPI1 slave for the WuaDVI control link (boot handshake).
 *
 * Implements the slave side of link_protocol.h: answers PING with a PONG
 * carrying firmware version, current mode and compiled-in resolution, and
 * latches the DISPLAY_START command for the main loop to act on.
 *
 * Runs by polling the PL022 RX FIFO — no DMA.  The link is 1 MHz with
 * 32-byte packets, so servicing it from loop() leaves margin of several
 * orders of magnitude.  The same physical pins are later re-used by the
 * framebuffer rect receiver (spi_slave.h); call link_slave_deinit() before
 * handing the bus over.
 *
 * Pins (WuaDVI PCB / RP2350-PiZero, SPI1): SCK=10, TX=11, RX=12, CS=13.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "link_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configure SPI1 as a mode-1 slave and start listening for commands.
 *
 * Starts in LINK_MODE_SPLASH.  Must be called before the first
 * link_slave_poll().
 */
void link_slave_init(void);

/**
 * @brief Release the SPI peripheral so another driver can claim the bus.
 *
 * Call once DISPLAY_START has been observed, right before spi_slave_init()
 * re-purposes the same pins for the framebuffer rect DMA receiver.
 */
void link_slave_deinit(void);

/**
 * @brief Service the control link: drain the RX FIFO and process packets.
 *
 * For every complete, checksum-valid packet:
 *   - PING           → preloads the PONG response into the TX FIFO.
 *   - DISPLAY_START  → latches the display request (only in LINK_MODE_READY).
 *
 * Call from loop() as often as possible while the link is active.
 */
void link_slave_poll(void);

/**
 * @brief Set the mode reported in the PONG response.
 * @param mode  LINK_MODE_SPLASH or LINK_MODE_READY.
 */
void link_slave_set_mode(uint8_t mode);

/**
 * @brief Check (without clearing) whether a DISPLAY_START command arrived.
 * @return true once a valid DISPLAY_START was received in READY mode.
 */
bool link_slave_display_requested(void);

/** @return Packets accepted since boot (checksum valid). */
uint32_t link_slave_packets_ok(void);

/** @return Packets rejected since boot (checksum mismatch). */
uint32_t link_slave_packets_bad(void);

#ifdef __cplusplus
}
#endif
