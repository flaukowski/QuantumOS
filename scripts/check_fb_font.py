#!/usr/bin/env python3
"""Every character drawn by fb_banner()/fb_draw_string() must have a glyph.

The framebuffer font (kernel/src/fb.c) is deliberately minimal, and
glyph_for() silently renders missing characters as blanks — which is how
the field view's caption shipped reading "OST  E D  E" instead of
"GHOST FIELD LIVE" (found on the first real-hardware boot). This check
makes a missing glyph a build failure instead of a silent blank.

Exit 0 if every banner character is covered, 1 otherwise.
"""
import re
import sys

FB = "kernel/src/fb.c"

def main():
    src = open(FB, encoding="utf-8").read()

    glyphs = set(m.group(1) for m in re.finditer(r"\{'(.)',\s*\{", src))

    banners = set()
    for m in re.finditer(r'fb_banner\([^,]+,\s*"([^"]*)"', src):
        banners.update(m.group(1))
    for m in re.finditer(r'fb_draw_string\([^,]+,[^,]+,\s*"([^"]*)"', src):
        banners.update(m.group(1))

    missing = sorted(c for c in banners if c not in glyphs)
    if missing:
        print(f"ERROR: fb.c draws characters with no glyph in font[]: {missing}")
        print("Add the glyphs to font[] in kernel/src/fb.c (8 rows, MSB-left).")
        return 1
    print(f"OK: all {len(banners)} distinct banner characters have glyphs")
    return 0

if __name__ == "__main__":
    sys.exit(main())
