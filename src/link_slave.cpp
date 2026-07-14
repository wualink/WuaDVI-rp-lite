/**
 * @file link_slave.cpp
 * @brief Polled SPI1 slave for the WuaDVI control link — see link_slave.h.
 */
#include "link_slave.h"
#include "version.h"
#include "dvi_config.h"
#include "spi_slave.h"          /* WUADVI_SPI_PIN_* — same physical pins */
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include <string.h>

#define SPI_PORT  spi1

/* Compile-time mapping of the active resolution flag to its protocol id. */
#if defined(WUADVI_RES_800x600x1)
  #define LINK_RES_ID  LINK_RES_800x600x1
#elif defined(WUADVI_RES_640x480x1)
  #define LINK_RES_ID  LINK_RES_640x480x1
#elif defined(WUADVI_RES_400x240)
  #define LINK_RES_ID  LINK_RES_400x240
#elif defined(WUADVI_RES_320x240)
  #define LINK_RES_ID  LINK_RES_320x240
#else
  #error "No WUADVI_RES_* flag selected (see dvi_config.h)"
#endif

/* Packet reassembly state. */
static uint8_t  s_frame[LINK_PKT_SIZE];
static uint32_t s_idx = 0;

static uint8_t  s_mode = LINK_MODE_SPLASH;
static bool     s_display_requested = false;
static uint32_t s_ok = 0, s_bad = 0;

/**
 * @brief Raw PL022 slave bring-up shared by init and the PONG FIFO reload.
 *
 * Mode 1 (CPOL=0, CPHA=1), MSB first — CPHA=1 is the RP2040-E2 erratum
 * workaround for PL022 slave first-bit reception; the clock rate parameter
 * is ignored in slave mode (the master drives SCK at 1 MHz).
 */
static void spi_hw_init(void) {
    spi_init(SPI_PORT, 1000000u);
    spi_set_slave(SPI_PORT, true);
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);

    gpio_set_function(WUADVI_SPI_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(WUADVI_SPI_PIN_TX,  GPIO_FUNC_SPI);
    gpio_set_function(WUADVI_SPI_PIN_RX,  GPIO_FUNC_SPI);
    gpio_set_function(WUADVI_SPI_PIN_CS,  GPIO_FUNC_SPI);
    gpio_pull_up(WUADVI_SPI_PIN_CS);

    /* Drop bytes left over from a previous session. */
    while (spi_is_readable(SPI_PORT))
        (void)spi_get_hw(SPI_PORT)->dr;
}

void link_slave_init(void) {
    spi_hw_init();
    s_idx = 0;
    s_mode = LINK_MODE_SPLASH;
    s_display_requested = false;
}

void link_slave_deinit(void) {
    spi_deinit(SPI_PORT);
}

void link_slave_set_mode(uint8_t mode) { s_mode = mode; }

bool link_slave_display_requested(void) { return s_display_requested; }

uint32_t link_slave_packets_ok(void)  { return s_ok;  }
uint32_t link_slave_packets_bad(void) { return s_bad; }

/**
 * @brief Preload the 8-byte PONG response into the SPI TX FIFO.
 *
 * The PL022 has no TX-FIFO flush, so the peripheral is re-initialized to
 * empty it before loading the fresh response.  The master waits
 * LINK_PONG_DELAY_MS between the PING and the read-out, so the few
 * microseconds the port is down never overlap a transaction.
 */
static void load_pong(void) {
    uint8_t resp[LINK_RESP_SIZE] = {
        LINK_RMAGIC0, LINK_RMAGIC1,
        (uint8_t)WUADVI_FW_VERSION_MAJOR,
        (uint8_t)WUADVI_FW_VERSION_MINOR,
        (uint8_t)WUADVI_FW_VERSION_PATCH,
        s_mode,
        (uint8_t)LINK_RES_ID,
        0
    };
    resp[7] = link_checksum(resp, 7);

    spi_deinit(SPI_PORT);
    spi_hw_init();
    for (uint32_t i = 0; i < LINK_RESP_SIZE; ++i)
        spi_get_hw(SPI_PORT)->dr = resp[i];
    s_idx = 0;
}

/**
 * @brief Validate and act on one fully assembled 32-byte packet.
 */
static void handle_frame(void) {
    if (link_checksum(s_frame, LINK_PKT_SIZE - 1u) != s_frame[LINK_PKT_SIZE - 1u]) {
        ++s_bad;
        return;
    }
    ++s_ok;

    switch (s_frame[2]) {
    case LINK_TYPE_PING:
        load_pong();
        break;
    case LINK_TYPE_DISPLAY_START:
        /* Only honored once the splash minimum time has been served —
         * guarantees the branding screen is actually seen. */
        if (s_mode == LINK_MODE_READY)
            s_display_requested = true;
        break;
    default:
        break;
    }
}

void link_slave_poll(void) {
    while (spi_is_readable(SPI_PORT)) {
        uint8_t b = (uint8_t)spi_get_hw(SPI_PORT)->dr;

        /* Magic-based resynchronization: outside a frame, discard every byte
         * that does not start the A5 3C sequence (the zeros the master clocks
         * while reading a PONG all land here). */
        if (s_idx == 0 && b != LINK_MAGIC0) continue;
        if (s_idx == 1 && b != LINK_MAGIC1) { s_idx = (b == LINK_MAGIC0) ? 1u : 0u; continue; }

        s_frame[s_idx++] = b;
        if (s_idx >= LINK_PKT_SIZE) {
            s_idx = 0;
            handle_frame();
        }
    }
}
