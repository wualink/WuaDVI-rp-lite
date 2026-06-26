#include "spi_slave.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/dma.h"
#include <Arduino.h>
#include <string.h>

#define SPI_PORT      spi1
/*
 * Slave mode: spi_init() clock parameter is ignored (master drives SCK).
 * Set to 20 MHz as documentation for the expected master clock speed.
 * The ESP32 master must be configured to the same rate.
 */
#define SPI_SPEED_HZ  25000000u

/*
 * DMA RX buffer: one full rect packet (header + max payload).  DMA refills
 * it on every CS-low envelope from the ESP32.  At 20 MHz one packet takes
 * ~6.1 ms, and the RP2350 core1 finishes the per-rect blit in well under
 * that, so there's no race between blit and next DMA arrival.
 */
static uint8_t s_dma_rx_buf[RECT_TOTAL_SIZE];

/* TX buffer: slave sends zeros (master ignores MISO in this design). */
static uint8_t s_dma_tx_buf[RECT_TOTAL_SIZE];

static int                s_ch_rx = -1;
static int                s_ch_tx = -1;
static dma_channel_config s_cfg_rx;
static dma_channel_config s_cfg_tx;

/* Diagnostics counters — incremented from spi_slave_poll(). */
static volatile uint32_t s_stat_completions  = 0;
static volatile uint32_t s_stat_valid        = 0;
static volatile uint32_t s_stat_bad_magic    = 0;
static volatile uint32_t s_stat_unshifted    = 0;   /* recovered via 1-bit unshift */
static uint8_t           s_last_bad_hdr[4]   = {0};

static inline uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/*
 * 1-bit-shifted view of RECT_MAGIC, used to detect the PL022-slave bit-skew
 * condition.  When the slave's bit counter ends up offset by 1 against the
 * master (caused by a stray SCK edge during the ESP32's IO-matrix setup), every
 * received byte is the same data left-shifted by 1, with the next byte's MSB
 * filled in at the bottom.  Pre-computed:
 *
 *   0xA5 0xB6 0xC7 0xE9  →  0x4B 0x6D 0x8F 0xD2
 */
#define RECT_MAGIC_SHIFTED_0   0x4Bu
#define RECT_MAGIC_SHIFTED_1   0x6Du
#define RECT_MAGIC_SHIFTED_2   0x8Fu
#define RECT_MAGIC_SHIFTED_3   0xD2u

/*
 * Undo the 1-bit-late shift in place.  After this:
 *   buf[i]  =  ((old_buf[i-1] & 0x01) << 7) | (old_buf[i] >> 1)
 *
 * The MSB of buf[0] is lost forever (it was master's bit 0, never captured by
 * the slave because of the skew), so this is only safe to call when we've
 * already validated the shifted-magic pattern and know which 4 bytes occupy
 * the header — i.e. we don't depend on buf[0]'s MSB being correct.  The very
 * last bit of the buffer is set to 0 (no source bit available); since the
 * tail of the buffer is zero padding anyway, this is benign.
 */
static void unshift_in_place(uint8_t *buf, uint32_t n) {
    uint8_t prev_lsb = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t curr_lsb = (uint8_t)(buf[i] & 0x01u);
        buf[i] = (uint8_t)((prev_lsb << 7) | (buf[i] >> 1));
        prev_lsb = curr_lsb;
    }
}

static void restart_rx_dma(void) {
    dma_channel_configure(s_ch_rx, &s_cfg_rx,
        s_dma_rx_buf,              /* write: RAM buffer          */
        &spi_get_hw(SPI_PORT)->dr, /* read:  SPI RX FIFO         */
        RECT_TOTAL_SIZE,
        true);                     /* trigger immediately        */
}

static void restart_tx_dma(void) {
    dma_channel_configure(s_ch_tx, &s_cfg_tx,
        &spi_get_hw(SPI_PORT)->dr, /* write: SPI TX FIFO         */
        s_dma_tx_buf,              /* read:  RAM buffer (zeros)  */
        RECT_TOTAL_SIZE,
        true);                     /* trigger immediately        */
}

void spi_slave_init(void) {
    spi_init(SPI_PORT, SPI_SPEED_HZ);
    spi_set_slave(SPI_PORT, true);
    /* Mode 1 (CPOL=0, CPHA=1), MSB first — RP2040-E2 errata workaround
     * (slave mode requires CPHA=1 for reliable first-bit reception). */
    spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST);

    gpio_set_function(WUADVI_SPI_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(WUADVI_SPI_PIN_TX,  GPIO_FUNC_SPI);
    gpio_set_function(WUADVI_SPI_PIN_RX,  GPIO_FUNC_SPI);
    gpio_set_function(WUADVI_SPI_PIN_CS,  GPIO_FUNC_SPI);
    gpio_pull_up(WUADVI_SPI_PIN_CS);

    /* Flush stale bytes from any prior run. */
    while (spi_is_readable(SPI_PORT))
        (void)spi_get_hw(SPI_PORT)->dr;

    memset(s_dma_tx_buf, 0, sizeof(s_dma_tx_buf));

    s_ch_rx = dma_claim_unused_channel(true);
    s_ch_tx = dma_claim_unused_channel(true);

    /* RX: SPI FIFO → RAM, 8-bit, SPI RX DREQ, incrementing write address. */
    s_cfg_rx = dma_channel_get_default_config(s_ch_rx);
    channel_config_set_transfer_data_size(&s_cfg_rx, DMA_SIZE_8);
    channel_config_set_read_increment(&s_cfg_rx,  false);
    channel_config_set_write_increment(&s_cfg_rx, true);
    channel_config_set_dreq(&s_cfg_rx, spi_get_dreq(SPI_PORT, false));

    /* TX: RAM → SPI FIFO, 8-bit, SPI TX DREQ, incrementing read address. */
    s_cfg_tx = dma_channel_get_default_config(s_ch_tx);
    channel_config_set_transfer_data_size(&s_cfg_tx, DMA_SIZE_8);
    channel_config_set_read_increment(&s_cfg_tx,  true);
    channel_config_set_write_increment(&s_cfg_tx, false);
    channel_config_set_dreq(&s_cfg_tx, spi_get_dreq(SPI_PORT, true));

    restart_rx_dma();
    restart_tx_dma();
}

bool spi_slave_poll(void) {
    if (dma_channel_is_busy(s_ch_rx)) return false;

    ++s_stat_completions;

    bool clean_magic = (s_dma_rx_buf[0] == RECT_MAGIC_0 &&
                        s_dma_rx_buf[1] == RECT_MAGIC_1 &&
                        s_dma_rx_buf[2] == RECT_MAGIC_2 &&
                        s_dma_rx_buf[3] == RECT_MAGIC_3);

    bool shifted_magic = (s_dma_rx_buf[0] == RECT_MAGIC_SHIFTED_0 &&
                          s_dma_rx_buf[1] == RECT_MAGIC_SHIFTED_1 &&
                          s_dma_rx_buf[2] == RECT_MAGIC_SHIFTED_2 &&
                          s_dma_rx_buf[3] == RECT_MAGIC_SHIFTED_3);

    /* PL022 1-bit-skew workaround: if the buffer matches the shifted magic
     * pattern, un-shift the entire packet so the coords and pixels end up
     * master-aligned for the blit. */
    if (shifted_magic) {
        unshift_in_place(s_dma_rx_buf, RECT_TOTAL_SIZE);
        ++s_stat_unshifted;
    }

    bool valid = clean_magic || shifted_magic;

    if (valid) {
        ++s_stat_valid;
    } else {
        ++s_stat_bad_magic;
        s_last_bad_hdr[0] = s_dma_rx_buf[0];
        s_last_bad_hdr[1] = s_dma_rx_buf[1];
        s_last_bad_hdr[2] = s_dma_rx_buf[2];
        s_last_bad_hdr[3] = s_dma_rx_buf[3];
    }

    /* Re-arm both channels regardless of validity so the next packet is never
     * missed.  The RX DMA is gated on the SPI RX DREQ, so s_dma_rx_buf is not
     * touched until the ESP32 starts the next CS-low transaction (typically
     * several ms away due to LVGL + SPI setup), which leaves the caller plenty
     * of time to blit before the buffer gets overwritten. */
    restart_rx_dma();
    restart_tx_dma();

    return valid;
}

uint32_t spi_slave_completions(void)  { return s_stat_completions;  }
uint32_t spi_slave_valid_packets(void){ return s_stat_valid;        }
uint32_t spi_slave_bad_magic(void)    { return s_stat_bad_magic;    }
uint32_t spi_slave_unshifted(void)    { return s_stat_unshifted;    }
uint32_t spi_slave_dma_remaining(void){
    if (s_ch_rx < 0) return 0;
    return dma_channel_hw_addr(s_ch_rx)->transfer_count;
}
const uint8_t *spi_slave_last_bad_header(void) { return s_last_bad_hdr; }

void spi_slave_blit_rect(uint8_t *dvi_fb) {
    if (dvi_fb == nullptr) return;

#if defined(WUADVI_COLOR_MONO)
    /* Monochrome (640x480x1): the payload is 1 bit/pixel, packed MSB-first with
     * each rect row padded to a whole byte.  Unpack into the GFXcanvas1 buffer
     * (also MSB-first, stride = ceil(SCREEN_W/8)) one pixel at a time, since the
     * destination x is generally not byte-aligned. */
    const uint16_t x1 = read_u16_le(s_dma_rx_buf + 4);
    const uint16_t y1 = read_u16_le(s_dma_rx_buf + 6);
    const uint16_t x2 = read_u16_le(s_dma_rx_buf + 8);
    const uint16_t y2 = read_u16_le(s_dma_rx_buf + 10);

    if (x2 < x1 || y2 < y1)               return;
    if (x2 >= SCREEN_W || y2 >= SCREEN_H) return;

    const uint32_t rect_w   = (uint32_t)(x2 - x1 + 1);
    const uint32_t rect_h   = (uint32_t)(y2 - y1 + 1);
    const uint32_t row_bytes = (rect_w + 7u) / 8u;
    if (row_bytes * rect_h > RECT_PAYLOAD_MAX) return;

    const uint8_t *src       = s_dma_rx_buf + RECT_HEADER_SIZE;
    const uint32_t fb_stride = ((uint32_t)SCREEN_W + 7u) / 8u;

    for (uint32_t row = 0; row < rect_h; ++row) {
        const uint8_t *srow = src + row * row_bytes;
        uint8_t       *frow = dvi_fb + (uint32_t)(y1 + row) * fb_stride;
        for (uint32_t col = 0; col < rect_w; ++col) {
            const uint8_t bit  = (uint8_t)((srow[col >> 3] >> (7 - (col & 7))) & 1u);
            const uint32_t x   = (uint32_t)x1 + col;
            uint8_t       *fb  = frow + (x >> 3);
            const uint8_t  msk = (uint8_t)(0x80u >> (x & 7));
            if (bit) *fb |= msk; else *fb &= (uint8_t)~msk;
        }
    }
    return;
#else

    const uint16_t x1 = read_u16_le(s_dma_rx_buf + 4);
    const uint16_t y1 = read_u16_le(s_dma_rx_buf + 6);
    const uint16_t x2 = read_u16_le(s_dma_rx_buf + 8);
    const uint16_t y2 = read_u16_le(s_dma_rx_buf + 10);

    /* Reject malformed coords silently; the next packet may resync. */
    if (x2 < x1 || y2 < y1)              return;
    if (x2 >= SCREEN_W || y2 >= SCREEN_H) return;

    /* Format-agnostic blit: WUADVI_PIXEL_BYTES is 2 (RGB565).  Both source
     * payload and destination framebuffer use the same bytes-per-pixel, so the
     * copy is identical apart from the stride. */
    const uint32_t rect_w  = (uint32_t)(x2 - x1 + 1);
    const uint32_t rect_h  = (uint32_t)(y2 - y1 + 1);
    const uint32_t row_bytes = rect_w * WUADVI_PIXEL_BYTES;
    if (rect_w * rect_h * WUADVI_PIXEL_BYTES > RECT_PAYLOAD_MAX) return;

    const uint8_t  *src = s_dma_rx_buf + RECT_HEADER_SIZE;
    uint8_t        *dst = dvi_fb + ((uint32_t)y1 * SCREEN_W + x1) * WUADVI_PIXEL_BYTES;
    const uint32_t  dst_stride_bytes = (uint32_t)SCREEN_W * WUADVI_PIXEL_BYTES;

    for (uint32_t row = 0; row < rect_h; ++row) {
        memcpy(dst, src, row_bytes);
        src += row_bytes;
        dst += dst_stride_bytes;
    }
#endif /* WUADVI_COLOR_MONO */
}
