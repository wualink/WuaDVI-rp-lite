#include <Arduino.h>
#include "hardware/pio.h"
#include "hardware/watchdog.h"
#include <PicoDVI.h>
#include "dvi_config.h"

#if defined(WUADVI_RES_800x600x1)
#include "hardware/structs/qmi.h"

/* Raise the QMI flash clock divider so QSPI/XIP stays within the flash's
 * rated speed once PicoDVI raises the sysclock to 354 MHz (354/4 ≈ 88 MHz;
 * the bootrom divider of 3 would give 118 MHz, too close to the limit and
 * set assuming a far lower sysclock).  In RAM: XIP fetches mid-write would
 * be performed with the very timing being changed.  Flash reads at this
 * setting were verified by checksum against 150 MHz during bring-up. */
static void __not_in_flash_func(qmi_flash_clkdiv)(uint32_t clkdiv) {
    qmi_hw->m[0].timing = (qmi_hw->m[0].timing & ~QMI_M0_TIMING_CLKDIV_BITS)
                        | (clkdiv << QMI_M0_TIMING_CLKDIV_LSB);
    __compiler_memory_barrier();
}
#endif

#ifdef SERIAL_TEST
#include "serial_test.h"
#else
#include "spi_slave.h"
#endif

/* ------------------------------------------------------------------ */
/* DVI display                                                          */
/* ------------------------------------------------------------------ */
/* WuaDVI is DVIGFX16 (RGB565) or DVIGFX1 (mono), chosen by the
 * resolution flag in platformio.ini.  See dvi_config.h. */
WuaDVI dvi_display(DVI_RESOLUTION, DVI_BOARD_CFG);

/* Non-blocking Serial wrapper — a blocked USB-CDC write can stall ~1 ms,
 * long enough to delay PicoDVI's DMA-chain interrupts (period ≈ 69 µs/line). */
static void safe_print(const char *str) {
    int n = (int)strlen(str);
    if (Serial.availableForWrite() >= n)
        Serial.write(str, n);
}

template<typename... Args>
static void safe_printf(const char *fmt, Args... args) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), fmt, args...);
    if (n > 0 && Serial.availableForWrite() >= n)
        Serial.write(buf, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* Arduino entry points                                                 */
/* ------------------------------------------------------------------ */
void setup(void) {
    Serial.begin(115200);
    delay(3000);
    safe_print("[OK] Serial started\n");

    /* Required for RP2350-PiZero: HDMI GPIOs 32-38 are above the default
     * PIO window (0-31).  Shift PIO0's window to 16-47 to reach them. */
    pio_set_gpio_base(pio0, 16);

#if defined(WUADVI_RES_800x600x1)
    /* Must run BEFORE dvi_display.begin() raises the sysclock to 354 MHz. */
    qmi_flash_clkdiv(4u);
#endif

    if (!dvi_display.begin()) {
        safe_print("[ERROR] DVI init failed\n");
        while (true) tight_loop_contents();
    }
    safe_print("[OK] DVI started\n");

#ifdef SERIAL_TEST
    /* Yellow screen: indicates Serial test mode is active. */
    dvi_display.fillScreen(dvi_display.color565(128, 128, 0));
    serial_test_init();
    safe_printf("[MODE] SERIAL_TEST — frame=%lu B  send with test/send_frame.py\n",
                (unsigned long)FRAME_TOTAL_SIZE);
#else
    /* Blue screen: waiting for ESP32 framebuffer over SPI. */
    dvi_display.fillScreen(dvi_display.color565(0, 0, 128));
    spi_slave_init();
    safe_printf("[MODE] SPI slave — SCK=%u RX=%u TX=%u CS=%u  frame=%lu B\n",
                WUADVI_SPI_PIN_SCK, WUADVI_SPI_PIN_RX,
                WUADVI_SPI_PIN_TX,  WUADVI_SPI_PIN_CS,
                (unsigned long)FRAME_TOTAL_SIZE);
#endif

#if defined(WUADVI_COLOR_MONO)
    /* Boot/waiting screen for mono: a static pattern that confirms the native
     * signal locks before the ESP32 connects.  Once rects arrive they blit
     * over it (1-bit packed; see spi_slave_blit_rect). */
    dvi_display.drawTestPattern();
    safe_printf("[MODE] MONO %dx%d — 1-bit SPI, waiting for ESP32\n",
                SCREEN_W, SCREEN_H);
#endif

    watchdog_enable(8000, true);
    safe_print("[OK] Setup complete — waiting for framebuffer\n");
}

void loop(void) {
    watchdog_update();

    bool packet_received = false;

#ifdef SERIAL_TEST
    /* getBuffer() is uint16_t* (RGB565) or uint8_t* (mono, bit-packed); the
     * blit/test helpers work in raw bytes, so view it as bytes regardless of mode. */
    packet_received = serial_test_poll((uint8_t *)dvi_display.getBuffer());
#else
    if (spi_slave_poll()) {
        /* Partial-rect mode: each packet is one dirty rect from the ESP32's
         * LVGL flush.  Blit it into the right position of the DVI framebuffer;
         * untouched pixels keep their previous content. */
        spi_slave_blit_rect((uint8_t *)dvi_display.getBuffer());
        packet_received = true;
    }
#endif

    if (packet_received) {
        static uint32_t s_packet_count = 0;
        ++s_packet_count;

        static uint32_t s_last_log_ms = 0;
        uint32_t now = millis();
        if (now - s_last_log_ms >= 2000u) {
            s_last_log_ms = now;
            safe_printf("[DVI] packets received: %lu\n", (unsigned long)s_packet_count);
        }
    }

    static uint32_t s_last_hb_ms = 0;
    uint32_t now = millis();
    if (now - s_last_hb_ms >= 2000u) {
        s_last_hb_ms = now;
#ifdef SERIAL_TEST
        safe_print("[ALIVE]\n");
#else
        const uint8_t *bh = spi_slave_last_bad_header();
        safe_printf("[SPI] dma_done=%lu  valid=%lu  unshifted=%lu  bad_magic=%lu  "
                    "dma_remaining=%lu/%lu  last_bad_hdr=%02X %02X %02X %02X\n",
                    (unsigned long)spi_slave_completions(),
                    (unsigned long)spi_slave_valid_packets(),
                    (unsigned long)spi_slave_unshifted(),
                    (unsigned long)spi_slave_bad_magic(),
                    (unsigned long)spi_slave_dma_remaining(),
                    (unsigned long)RECT_TOTAL_SIZE,
                    bh[0], bh[1], bh[2], bh[3]);
#endif
    }
}
