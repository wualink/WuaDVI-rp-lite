#include <Arduino.h>
#include "hardware/pio.h"
#include "hardware/watchdog.h"
#include <PicoDVI.h>
#include "dvi_config.h"

#ifdef SERIAL_TEST
#include "serial_test.h"
#else
#include "spi_slave.h"
#endif

/* ------------------------------------------------------------------ */
/* DVI display                                                          */
/* ------------------------------------------------------------------ */
DVIGFX16 dvi_display(DVI_RESOLUTION, DVI_BOARD_CFG);

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

    watchdog_enable(8000, true);
    safe_print("[OK] Setup complete — waiting for framebuffer\n");
}

void loop(void) {
    watchdog_update();

    bool packet_received = false;

#ifdef SERIAL_TEST
    packet_received = serial_test_poll(dvi_display.getBuffer());
#else
    if (spi_slave_poll()) {
        /* Partial-rect mode: each packet is one dirty rect from the ESP32's
         * LVGL flush.  Blit it into the right position of the DVI framebuffer;
         * untouched pixels keep their previous content. */
        spi_slave_blit_rect(dvi_display.getBuffer());
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
