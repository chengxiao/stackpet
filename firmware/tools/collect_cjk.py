#!/usr/bin/env python3
"""Collect every CJK character and full-width punctuation used in the given source trees.

Used by gen_stackpet_fonts.sh to build the subset title fonts. Only C/C++ sources are scanned, and the
generated font directory is skipped (its glyph comments would otherwise feed the font back into itself).

Usage:
  collect_cjk.py DIR...                     print all collected characters
  collect_cjk.py --in-font FONT.ttf DIR...  print only the characters FONT.ttf contains
  collect_cjk.py --not-in-font FONT.ttf DIR...  print only the characters FONT.ttf lacks
"""
import os
import struct
import sys

SOURCE_EXTS = {'.h', '.hpp', '.c', '.cc', '.cpp'}
SKIP_DIRS = {'fonts'}

RANGES = [
    (0x00B7, 0x00B7),  # middle dot
    (0x2010, 0x206F),  # general punctuation: dashes, quotes, ellipsis
    (0x3000, 0x303F),  # CJK symbols and punctuation
    (0x3400, 0x4DBF),  # CJK extension A
    (0x4E00, 0x9FFF),  # CJK unified ideographs
    (0xFF00, 0xFFEF),  # full-width forms
]


def wanted(ch):
    cp = ord(ch)
    return any(lo <= cp <= hi for lo, hi in RANGES)


def collect(dirs):
    chars = set()
    for root_dir in dirs:
        if not os.path.exists(root_dir):
            continue
        for root, subdirs, files in os.walk(root_dir):
            subdirs[:] = [d for d in subdirs if d not in SKIP_DIRS]
            for name in files:
                if os.path.splitext(name)[1] not in SOURCE_EXTS:
                    continue
                with open(os.path.join(root, name), encoding='utf-8', errors='ignore') as f:
                    chars.update(ch for ch in f.read() if wanted(ch))
    return chars


def font_codepoints(path):
    """Unicode code points mapped by a TrueType/OpenType font (cmap format 4 and 12)."""
    data = open(path, 'rb').read()
    num_tables = struct.unpack('>H', data[4:6])[0]
    cmap_offset = None
    for i in range(num_tables):
        tag, _, offset, _ = struct.unpack('>4sIII', data[12 + i * 16:28 + i * 16])
        if tag == b'cmap':
            cmap_offset = offset
    if cmap_offset is None:
        raise ValueError(f'{path}: no cmap table')

    points = set()
    num_subtables = struct.unpack('>H', data[cmap_offset + 2:cmap_offset + 4])[0]
    for i in range(num_subtables):
        platform, encoding, sub_offset = struct.unpack('>HHI', data[cmap_offset + 4 + i * 8:cmap_offset + 12 + i * 8])
        if platform not in (0, 3):
            continue
        base = cmap_offset + sub_offset
        fmt = struct.unpack('>H', data[base:base + 2])[0]
        if fmt == 4:
            seg_x2 = struct.unpack('>H', data[base + 6:base + 8])[0]
            seg = seg_x2 // 2
            ends = struct.unpack(f'>{seg}H', data[base + 14:base + 14 + seg_x2])
            starts_off = base + 16 + seg_x2
            starts = struct.unpack(f'>{seg}H', data[starts_off:starts_off + seg_x2])
            deltas = struct.unpack(f'>{seg}h', data[starts_off + seg_x2:starts_off + 2 * seg_x2])
            range_off_pos = starts_off + 2 * seg_x2
            range_offs = struct.unpack(f'>{seg}H', data[range_off_pos:range_off_pos + seg_x2])
            for s in range(seg):
                for cp in range(starts[s], ends[s] + 1):
                    if cp == 0xFFFF:
                        continue
                    if range_offs[s] == 0:
                        glyph = (cp + deltas[s]) & 0xFFFF
                    else:
                        addr = range_off_pos + s * 2 + range_offs[s] + (cp - starts[s]) * 2
                        glyph = struct.unpack('>H', data[addr:addr + 2])[0]
                        if glyph:
                            glyph = (glyph + deltas[s]) & 0xFFFF
                    if glyph:
                        points.add(cp)
        elif fmt == 12:
            groups = struct.unpack('>I', data[base + 12:base + 16])[0]
            for g in range(groups):
                start, end, glyph = struct.unpack('>III', data[base + 16 + g * 12:base + 28 + g * 12])
                for cp in range(start, end + 1):
                    if glyph + (cp - start):
                        points.add(cp)
    return points


def main():
    args = sys.argv[1:]
    mode = None
    font = None
    if args and args[0] in ('--in-font', '--not-in-font'):
        mode, font, args = args[0], args[1], args[2:]

    chars = collect(args)
    if mode is not None:
        covered = font_codepoints(font)
        if mode == '--in-font':
            chars = {c for c in chars if ord(c) in covered}
        else:
            chars = {c for c in chars if ord(c) not in covered}
    sys.stdout.write(''.join(sorted(chars)))


if __name__ == '__main__':
    main()
