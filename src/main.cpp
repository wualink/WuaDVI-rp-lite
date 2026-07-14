/**
 * @file main.cpp
 * @brief WuaDVI RP2354B firmware — DVI display engine entry point.
 *
 * The RP2354B is the display engine of the WuaDVI board: it generates the
 * DVI/HDMI signal with PicoDVI and renders whatever the ESP32-C3 application
 * processor sends over SPI.  Boot flow:
 *
 *   1. DVI starts and the boot splash is shown (organization, product,
 *      firmware version, resolution, animated "Loading").
 *   2. The control link (link_slave, 32-byte packets) answers the ESP32's
 *      PING with version/mode/resolution, letting it decide whether this
 *      firmware must be re-flashed (ROM UART boot) before continuing.
 *   3. After the splash minimum time the firmware reports READY; when the
 *      ESP32 sends DISPLAY_START the SPI bus is re-armed as the fixed-size
 *      framebuffer-rect DMA receiver (spi_slave).
 *   4. From then on every received rect is blitted into the PicoDVI
 *      framebuffer — the ESP32 fully owns the screen.
 *
 * Build-time options (platformio.ini):
 *   - WUADVI_RES_*   exactly one resolution flag (see dvi_config.h).
 *   - SERIAL_TEST    replaces the SPI pipeline with the USB-serial frame
 *                    test harness (test/send_frame.py); no splash/link.
 */
#include <Arduino.h>
#include "hardware/pio.h"
#include "hardware/watchdog.h"
#include <PicoDVI.h>
#include "dvi_config.h"
#include "version.h"

#if defined(WUADVI_RES_800x600x1)
#include "hardware/structs/qmi.h"

/**
 * @brief Raise the QMI flash clock divider so QSPI/XIP stays within the
 *        flash's rated speed once PicoDVI raises the sysclock to 354 MHz.
 *
 * 354/4 ≈ 88 MHz; the bootrom divider of 3 would give 118 MHz, too close to
 * the limit and chosen assuming a far lower sysclock.  Runs from RAM: XIP
 * fetches mid-write would otherwise be performed with the very timing being
 * changed.  Flash reads at this setting were verified by checksum against
 * 150 MHz during bring-up.  Do not change without re-validating on hardware.
 *
 * @param clkdiv  New QMI M0 clock divider.
 */
static void __not_in_flash_func(qmi_flash_clkdiv)(uint32_t clkdiv) {
    qmi_hw->m[0].timing = (qmi_hw->m[0].timing & ~QMI_M0_TIMING_CLKDIV_BITS)
                        | (clkdiv << QMI_M0_TIMING_CLKDIV_LSB);
    __compiler_memory_barrier();
}
#endif

#ifdef SERIAL_TEST
#include "serial_test.h"
#else
#include "splash.h"
#include "link_slave.h"
#include "spi_slave.h"
#endif

/* ------------------------------------------------------------------ */
/* DVI display                                                          */
/* ------------------------------------------------------------------ */
/** The display object — DVIGFX16 (RGB565) or DVIGFX1 (mono), selected by the
 *  resolution flag in platformio.ini.  See dvi_config.h. */
WuaDVI dvi_display(DVI_RESOLUTION, DVI_BOARD_CFG);

/* ------------------------------------------------------------------ */
/* Non-blocking serial logging                                          */
/* ------------------------------------------------------------------ */
/**
 * @brief Print a string only if it fits the USB-CDC buffer right now.
 *
 * A blocked USB-CDC write can stall ~1 ms — long enough to delay PicoDVI's
 * DMA-chain interrupts (period ≈ 69 µs/line) and glitch the picture, so
 * logging must never wait for the host.
 *
 * @param str  NUL-terminated string to print.
 */
static void safe_print(const char *str) {
    int n = (int)strlen(str);
    if (Serial.availableForWrite() >= n)
        Serial.write(str, n);
}

/**
 * @brief printf-style variant of safe_print() (128-byte line limit).
 * @param fmt  printf format string.
 * @param args Format arguments.
 */
template<typename... Args>
static void safe_printf(const char *fmt, Args... args) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf), fmt, args...);
    if (n > 0 && Serial.availableForWrite() >= n)
        Serial.write(buf, (size_t)n);
}

#ifndef SERIAL_TEST
/* ------------------------------------------------------------------ */
/* Boot state machine                                                   */
/* ------------------------------------------------------------------ */
/** Firmware lifecycle states — see the file header for the flow. */
enum class BootState : uint8_t {
    Splash,   /**< Splash on screen, minimum display time not yet served. */
    Ready,    /**< Splash served; waiting for DISPLAY_START from the ESP32. */
    Display   /**< Rect DMA armed; the ESP32 owns the screen. */
};

static BootState s_state = BootState::Splash;
static uint32_t  s_splash_start_ms = 0;

/**
 * @brief Handle the SPLASH state: animate, serve pings, honor the timer.
 *
 * Transitions to READY once WUADVI_SPLASH_MS has elapsed.
 */
static void state_splash(void) {
    link_slave_poll();
    splash_tick(dvi_display);

    if (millis() - s_splash_start_ms >= WUADVI_SPLASH_MS) {
        link_slave_set_mode(LINK_MODE_READY);
        s_state = BootState::Ready;
        safe_print("[STATE] READY - waiting for DISPLAY_START\n");
    }
}

/**
 * @brief Handle the READY state: keep serving the link until the ESP32
 *        requests the display stream.
 *
 * On DISPLAY_START: releases the control link, arms the framebuffer-rect
 * DMA receiver on the same SPI pins and transitions to DISPLAY.
 */
static void state_ready(void) {
    link_slave_poll();
    splash_tick(dvi_display);

    if (link_slave_display_requested()) {
        link_slave_deinit();
        spi_slave_init();
        s_state = BootState::Display;
        safe_printf("[STATE] DISPLAY - rect DMA armed (frame=%lu B)\n",
                    (unsigned long)RECT_TOTAL_SIZE);
    }
}

/**
 * @brief Handle the DISPLAY state: blit every received rect and log stats.
 */
static void state_display(void) {
    if (spi_slave_poll()) {
        /* Each packet is one dirty rect from the ESP32's LVGL flush.  Blit it
         * into position; untouched pixels keep their previous content. */
        spi_slave_blit_rect((uint8_t *)dvi_display.getBuffer());

        static uint32_t s_rect_count = 0;
        static uint32_t s_last_log_ms = 0;
        ++s_rect_count;
        uint32_t now = millis();
        if (now - s_last_log_ms >= 2000u) {
            s_last_log_ms = now;
            safe_printf("[DVI] rects blitted: %lu\n", (unsigned long)s_rect_count);
        }
    }
}

/**
 * @brief Emit a periodic per-state heartbeat with link/DMA diagnostics.
 */
static void heartbeat(void) {
    static uint32_t s_last_hb_ms = 0;
    uint32_t now = millis();
    if (now - s_last_hb_ms < 2000u) return;
    s_last_hb_ms = now;

    switch (s_state) {
    case BootState::Splash:
    case BootState::Ready:
        safe_printf("[LINK] %s ok=%lu bad=%lu\n",
                    s_state == BootState::Splash ? "SPLASH" : "READY",
                    (unsigned long)link_slave_packets_ok(),
                    (unsigned long)link_slave_packets_bad());
        break;
    case BootState::Display: {
        const uint8_t *bh = spi_slave_last_bad_header();
        safe_printf("[SPI] dma_done=%lu valid=%lu unshifted=%lu bad_magic=%lu "
                    "remaining=%lu/%lu last_bad_hdr=%02X %02X %02X %02X\n",
                    (unsigned long)spi_slave_completions(),
                    (unsigned long)spi_slave_valid_packets(),
                    (unsigned long)spi_slave_unshifted(),
                    (unsigned long)spi_slave_bad_magic(),
                    (unsigned long)spi_slave_dma_remaining(),
                    (unsigned long)RECT_TOTAL_SIZE,
                    bh[0], bh[1], bh[2], bh[3]);
        break;
    }
    }
}
#endif /* !SERIAL_TEST */

/* ------------------------------------------------------------------ */
/* Arduino entry points                                                 */
/* ------------------------------------------------------------------ */
/**
 * @brief One-time initialization: DVI, splash and the control link.
 */
void setup(void) {
    Serial.begin(115200);

    /* Required for the WuaDVI board: HDMI GPIOs 32-38 are above the default
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

#ifdef SERIAL_TEST
    /* Yellow screen: indicates the USB-serial test harness is active. */
    dvi_display.fillScreen(dvi_display.color565(128, 128, 0));
    serial_test_init();
    safe_printf("[MODE] SERIAL_TEST - frame=%lu B, send with test/send_frame.py\n",
                (unsigned long)FRAME_TOTAL_SIZE);
#else
    splash_draw(dvi_display);
    link_slave_init();
    s_splash_start_ms = millis();
    safe_print("[BOOT] WuaDVI RP firmware v" WUADVI_FW_VERSION_STRING
               " - splash + control link up\n");
#endif

    watchdog_enable(8000, true);
}

/**
 * @brief Main loop: feed the watchdog and run the active state handler.
 */
void loop(void) {
    watchdog_update();

#ifdef SERIAL_TEST
    (void)serial_test_poll((uint8_t *)dvi_display.getBuffer());

    static uint32_t s_last_hb_ms = 0;
    uint32_t now = millis();
    if (now - s_last_hb_ms >= 2000u) {
        s_last_hb_ms = now;
        safe_print("[ALIVE]\n");
    }
#else
    switch (s_state) {
    case BootState::Splash:  state_splash();  break;
    case BootState::Ready:   state_ready();   break;
    case BootState::Display: state_display(); break;
    }
    heartbeat();
#endif
}
