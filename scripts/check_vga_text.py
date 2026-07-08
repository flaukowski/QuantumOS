#!/usr/bin/env python3
"""Assert a string is visible in the VGA text screen of a running QEMU guest.

Reads the 80x25 text cells at 0xB8000 through the QEMU human monitor
(unix socket) with `xp`, decodes the character bytes (every even byte of
each 2-byte cell), and greps for the needle. This checks what is
actually ON SCREEN — the display a serial-less machine's operator sees —
not what went down the serial port.

Usage: check_vga_text.py <monitor-socket> <needle>
Exit 0 if the needle is on screen, 1 otherwise.
"""
import re
import socket
import sys
import time

SCREEN_BYTES = 80 * 25 * 2  # one visible text page

def main():
    sock_path, needle = sys.argv[1], sys.argv[2]
    s = socket.socket(socket.AF_UNIX)
    s.connect(sock_path)
    time.sleep(0.2)
    s.sendall(f"xp /{SCREEN_BYTES}bx 0xb8000\n".encode())
    time.sleep(1.5)
    data = b""
    s.settimeout(1.0)
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    except OSError:
        pass
    s.close()

    # Monitor lines look like: 00000000000b8000: 0x51 0x07 0x75 0x07 ...
    raw = bytearray()
    for m in re.finditer(rb"[0-9a-f]+:((?: 0x[0-9a-f]{2})+)", data):
        for b in m.group(1).split():
            raw.append(int(b, 16))
    chars = bytes(raw[i] for i in range(0, len(raw), 2))  # drop attribute bytes
    text = chars.decode("cp437", "replace")

    if needle in text:
        print(f"OK: '{needle}' is on the VGA screen")
        return 0
    print(f"MISSING: '{needle}' not found on the VGA screen ({len(raw)//2} cells read)")
    # show the screen for diagnosis, 80 chars per row
    for row in range(0, len(text), 80):
        line = text[row:row + 80].rstrip()
        if line:
            print(f"| {line}")
    return 1

if __name__ == "__main__":
    sys.exit(main())
