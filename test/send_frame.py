#!/usr/bin/env python3
"""
send_frame.py — WuaDVI-rp-lite Serial Test
===========================================
Sends one or more RGB565 framebuffers over USB Serial to the RP2350 running
in SERIAL_TEST mode.  The RP2350 validates the 4-byte magic header and copies
the pixel data directly into its PicoDVI framebuffer.

Requirements:
    pip install pyserial
    pip install pillow     # only needed for --image

Usage examples:
    python send_frame.py --port COM3
    python send_frame.py --port COM3 --pattern gradient --count 5
    python send_frame.py --port COM3 --pattern checkerboard
    python send_frame.py --port COM3 --image photo.png
    python send_frame.py --port /dev/ttyACM0 --pattern colorbar

    # Continuous loop (Ctrl+C to stop):
    python send_frame.py --port COM3 --count 0

Notes:
    - Close any serial monitor (pio device monitor, PuTTY, etc.) before running.
      Only one program can hold the serial port at a time.
    - Baud rate is irrelevant for USB-CDC; the RP2350 receives at USB Full Speed
      (~1 MB/s), so a full 320x240 frame transfers in ~150 ms (~6 fps).
    - If colors look byte-swapped on screen, change BYTE_ORDER below from
      '<' (little-endian) to '>' (big-endian) and try again.
"""

import argparse
import struct
import sys
import time

# ── Protocol constants (must match frame_config.h) ─────────────────────────
SCREEN_W     = 320
SCREEN_H     = 240
MAGIC        = bytes([0xA5, 0xB6, 0xC7, 0xD8])
PIXEL_BYTES  = SCREEN_W * SCREEN_H * 2          # 153 600
FRAME_BYTES  = len(MAGIC) + PIXEL_BYTES         # 153 604

# RGB565 byte order sent over the wire.
# '<' = little-endian (low byte first) — matches LVGL LV_COLOR_FORMAT_RGB565
# '>' = big-endian   (high byte first) — try this if colors look wrong
BYTE_ORDER = '<'

# ── Color helpers ────────────────────────────────────────────────────────────

def rgb_to_rgb565(r: int, g: int, b: int) -> bytes:
    """Pack an 8-bit R/G/B triplet into a 2-byte RGB565 word."""
    word = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return struct.pack(f'{BYTE_ORDER}H', word)


# ── Test pattern generators ──────────────────────────────────────────────────

def make_colorbar() -> bytes:
    """Classic 8-column SMPTE color bars."""
    columns = [
        (255, 255, 255),  # White
        (255, 255,   0),  # Yellow
        (  0, 255, 255),  # Cyan
        (  0, 255,   0),  # Green
        (255,   0, 255),  # Magenta
        (255,   0,   0),  # Red
        (  0,   0, 255),  # Blue
        (  0,   0,   0),  # Black
    ]
    bar_w = SCREEN_W // len(columns)
    buf = bytearray(PIXEL_BYTES)
    for y in range(SCREEN_H):
        for x in range(SCREEN_W):
            idx = min(x // bar_w, len(columns) - 1)
            offset = (y * SCREEN_W + x) * 2
            buf[offset:offset + 2] = rgb_to_rgb565(*columns[idx])
    return bytes(buf)


def make_gradient() -> bytes:
    """Horizontal red → blue gradient with green peak in the middle."""
    buf = bytearray(PIXEL_BYTES)
    for y in range(SCREEN_H):
        for x in range(SCREEN_W):
            t = x / (SCREEN_W - 1)
            r = int(255 * (1.0 - t))
            g = int(255 * (1.0 - abs(t - 0.5) * 2))
            b = int(255 * t)
            offset = (y * SCREEN_W + x) * 2
            buf[offset:offset + 2] = rgb_to_rgb565(r, g, b)
    return bytes(buf)


def make_checkerboard(square: int = 20) -> bytes:
    """Black-and-white checkerboard with configurable square size (pixels)."""
    buf = bytearray(PIXEL_BYTES)
    white = rgb_to_rgb565(255, 255, 255)
    black = rgb_to_rgb565(0, 0, 0)
    for y in range(SCREEN_H):
        for x in range(SCREEN_W):
            pixel = white if ((x // square) + (y // square)) % 2 == 0 else black
            offset = (y * SCREEN_W + x) * 2
            buf[offset:offset + 2] = pixel
    return bytes(buf)


def make_grid() -> bytes:
    """White grid lines every 10 pixels on a dark background — good for
    checking pixel alignment and any scaling/offset errors in the pipeline."""
    buf = bytearray(PIXEL_BYTES)
    bg    = rgb_to_rgb565(20, 20, 40)
    line  = rgb_to_rgb565(255, 255, 255)
    for y in range(SCREEN_H):
        for x in range(SCREEN_W):
            pixel = line if (x % 10 == 0 or y % 10 == 0) else bg
            offset = (y * SCREEN_W + x) * 2
            buf[offset:offset + 2] = pixel
    return bytes(buf)


def load_image(path: str) -> bytes:
    """Load any image, resize to 320×240, convert to RGB565."""
    try:
        from PIL import Image
    except ImportError:
        sys.exit("Pillow not installed.  Run:  pip install pillow")

    img = (Image.open(path)
               .convert('RGB')
               .resize((SCREEN_W, SCREEN_H), Image.LANCZOS))

    buf = bytearray(PIXEL_BYTES)
    pixels = list(img.getdata())
    for i, (r, g, b) in enumerate(pixels):
        buf[i * 2:i * 2 + 2] = rgb_to_rgb565(r, g, b)
    return bytes(buf)


# ── Transport ────────────────────────────────────────────────────────────────

def send_frames(port: str, pixels: bytes, count: int, delay_ms: int) -> None:
    try:
        import serial
    except ImportError:
        sys.exit("pyserial not installed.  Run:  pip install pyserial")

    assert len(pixels) == PIXEL_BYTES, \
        f"Expected {PIXEL_BYTES} pixel bytes, got {len(pixels)}"

    frame = MAGIC + pixels
    assert len(frame) == FRAME_BYTES

    print(f"Port        : {port}")
    print(f"Frame size  : {FRAME_BYTES:,} bytes  ({PIXEL_BYTES:,} pixels + {len(MAGIC)} magic)")
    print(f"Byte order  : {'little-endian (LE)' if BYTE_ORDER == '<' else 'big-endian (BE)'}")
    print(f"Frames      : {'continuous (Ctrl+C to stop)' if count == 0 else count}")
    print()

    with serial.Serial(port, baudrate=115200, timeout=5) as ser:
        time.sleep(0.5)          # let USB-CDC enumerate before first write

        frame_num = 0
        try:
            while count == 0 or frame_num < count:
                t0 = time.perf_counter()
                ser.write(frame)
                ser.flush()
                elapsed = time.perf_counter() - t0

                frame_num += 1
                fps = 1.0 / elapsed if elapsed > 0 else float('inf')
                print(f"  Frame {frame_num:>4}  {elapsed * 1000:6.0f} ms  "
                      f"({fps:.1f} fps)  "
                      f"{len(frame) / elapsed / 1024:.0f} KB/s")

                if delay_ms > 0 and (count == 0 or frame_num < count):
                    time.sleep(delay_ms / 1000.0)
        except KeyboardInterrupt:
            print("\nStopped by user.")

    print(f"\nDone — {frame_num} frame(s) sent.")


# ── CLI ──────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description='WuaDVI-rp-lite Serial Test — send RGB565 frames to RP2350')

    parser.add_argument(
        '--port', required=True,
        help='Serial port (e.g. COM3 on Windows, /dev/ttyACM0 on Linux)')

    parser.add_argument(
        '--pattern',
        choices=['colorbar', 'gradient', 'checkerboard', 'grid'],
        default='colorbar',
        help='Built-in test pattern (default: colorbar)')

    parser.add_argument(
        '--image',
        metavar='FILE',
        help='Image file (PNG/JPG/BMP). Resized to 320x240. Overrides --pattern.')

    parser.add_argument(
        '--count', type=int, default=1,
        help='Number of frames to send (0 = loop forever, default: 1)')

    parser.add_argument(
        '--delay', type=int, default=0,
        help='Extra delay between frames in ms (default: 0)')

    parser.add_argument(
        '--big-endian', action='store_true',
        help='Send RGB565 pixels big-endian (try if colors look byte-swapped)')

    args = parser.parse_args()

    global BYTE_ORDER
    if args.big_endian:
        BYTE_ORDER = '>'
        print("[INFO] Using big-endian byte order")

    # Build pixel buffer
    if args.image:
        print(f"Loading image: {args.image}")
        pixels = load_image(args.image)
    elif args.pattern == 'gradient':
        print("Generating gradient pattern...")
        pixels = make_gradient()
    elif args.pattern == 'checkerboard':
        print("Generating checkerboard pattern...")
        pixels = make_checkerboard()
    elif args.pattern == 'grid':
        print("Generating grid pattern...")
        pixels = make_grid()
    else:
        print("Generating color bar pattern...")
        pixels = make_colorbar()

    send_frames(args.port, pixels, args.count, args.delay)


if __name__ == '__main__':
    main()
