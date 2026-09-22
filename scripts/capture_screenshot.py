#!/usr/bin/env python3
"""Capture a real screenshot from the SenseCAP Indicator's running UI.

Sends the "screenshot" serial command, reads the raw RGB565 framebuffer
dump the firmware sends back (src/sensecap/main.cpp), and writes a PNG.
This is a genuine capture of what's physically on the panel, not a mockup.
"""
import argparse
import sys
import time

import serial
from PIL import Image


def capture(port: str, baud: int, out_path: str, timeout: float, goto: str | None = None) -> None:
    ser = serial.Serial(port, baud, timeout=timeout)
    ser.reset_input_buffer()

    if goto:
        ser.write(f"goto {goto}\n".encode())
        ser.flush()
        ack = ser.readline().decode(errors="replace").strip()
        if not ack.startswith("[GOTO]") or "Unbekannter" in ack:
            raise ValueError(f"goto '{goto}' fehlgeschlagen: '{ack}'")
        print(ack)
        time.sleep(0.3)  # kurz warten, bis LVGL den neuen Screen tatsaechlich gezeichnet hat

    ser.reset_input_buffer()
    ser.write(b"screenshot\n")
    ser.flush()

    width = height = None
    while True:
        line = ser.readline()
        if not line:
            raise TimeoutError("Kein 'SCREENSHOT_BEGIN' innerhalb des Timeouts empfangen.")
        text = line.decode(errors="replace").strip()
        if text.startswith("SCREENSHOT_BEGIN"):
            _, w, h = text.split()
            width, height = int(w), int(h)
            break

    byte_count = width * height * 2
    raw = bytearray()
    stall_deadline = time.monotonic() + timeout
    while len(raw) < byte_count:
        chunk = ser.read(byte_count - len(raw))
        if chunk:
            raw.extend(chunk)
            stall_deadline = time.monotonic() + timeout
        elif time.monotonic() > stall_deadline:
            raise IOError(f"Nur {len(raw)} von {byte_count} erwarteten Bytes erhalten (Uebertragung stecken geblieben).")

    end_line = ser.readline().decode(errors="replace").strip()
    if "SCREENSHOT_END" not in end_line:
        print(f"Warnung: kein SCREENSHOT_END-Marker gesehen ('{end_line}')", file=sys.stderr)

    ser.close()

    img = Image.new("RGB", (width, height))
    pixels = img.load()
    for y in range(height):
        row_offset = y * width * 2
        for x in range(width):
            i = row_offset + x * 2
            # RGB565, little-endian uint16 per pixel
            value = raw[i] | (raw[i + 1] << 8)
            r5 = (value >> 11) & 0x1F
            g6 = (value >> 5) & 0x3F
            b5 = value & 0x1F
            r8 = (r5 * 255) // 31
            g8 = (g6 * 255) // 63
            b8 = (b5 * 255) // 31
            pixels[x, y] = (r8, g8, b8)

    # Das Panel ist physisch auf dem Kopf montiert; main.cpp korrigiert das erst
    # beim Schreiben in den Framebuffer (siehe lvgl_disp_flush), das rohe
    # Speicherlayout selbst ist also gegenueber der menschlichen Sicht um 180
    # Grad gedreht. Hier zurueckdrehen, statt die Firmware anzufassen.
    img = img.transpose(Image.ROTATE_180)

    img.save(out_path)
    print(f"Gespeichert: {out_path} ({width}x{height})")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="Serieller Port, z.B. /dev/cu.usbmodemXXXX")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--out", required=True, help="Ziel-PNG-Datei")
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument(
        "--goto",
        help="Vorher per Serial-Befehl zu diesem Screen springen (siehe ui_model_test_goto_screen()), "
        "z.B. main, fire, confirm, settings. Ohne diese Option wird einfach der aktuell sichtbare Screen erfasst.",
    )
    args = parser.parse_args()
    capture(args.port, args.baud, args.out, args.timeout, args.goto)
