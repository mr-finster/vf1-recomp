#!/usr/bin/env python3
"""Build a Model 1 game's memory regions from its MAME set.

Reads the user's own ROM zip, checks every file's CRC32 and writes the regions MAME's driver
builds to build/<game>/<region>.bin. The output is ROM data: it stays in build/, which git ignores.

    tools/romset.py [--game NAME] roms/NAME.zip [build/NAME] [roms/model1io.zip ...]

The game is --game's, else the zip's name (vf.zip, vr.zip). The tables are the shared CPU board's
(here) and the game's own (tools/games/NAME/romset.py, a new game's is a new file there). Its
optional files (VF: 315-5724.bin, the TGP's own program from a current MAME set) are written
when found, and so is the I/O board's ROM (analog inputs).
"""
import importlib.util, os, sys, zipfile, zlib

# region name -> (size, fill)
COMMON_REGIONS = {
    'maincpu':      (0x2000000, 0xff),
    'copro_tables': (0x40000, 0x00),
    'other_data':   (0x80000, 0x00),
    'other_other_data': (0x20000, 0x00),
    'sndcpu':       (0xc0000, 0x00),
    'pcm1':         (0x400000, 0x00),
    'pcm2':         (0x400000, 0x00),
    'polygons':     (0x1000000, 0x00),
}

# (file, crc, region, offset, kind). kind: 'raw', 'byte16' (every other byte),
# 'word32' (16-bit words into every other 32-bit slot), 'wordswap' (16-bit byteswapped),
# 'byte32' (every fourth byte). The Model 1 CPU board's own ROMs, the same in every game's set;
# each game's are in tools/games/<game>/romset.py (ROMS, REGIONS past these, OPTIONAL files).
BOARD_ROMS = [
    ('opr14742.bin', 0x446a1085, 'copro_tables', 0x0, 'word32'),
    ('opr14743.bin', 0xe8953554, 'copro_tables', 0x2, 'word32'),
    ('opr14744.bin', 0x730ea9e0, 'other_data', 0x0, 'word32'),
    ('opr14745.bin', 0x4c934d96, 'other_data', 0x2, 'word32'),
    ('opr14746.bin', 0x2a266cbd, 'other_data', 0x40000, 'word32'),
    ('opr14747.bin', 0xa4ad5e19, 'other_data', 0x40002, 'word32'),
    ('opr14748.bin', 0x4a532cb8, 'other_other_data', 0x0, 'raw'),
]

GAMES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'games')

def games():
    return sorted(g for g in os.listdir(GAMES_DIR) if os.path.isfile(os.path.join(GAMES_DIR, g, 'romset.py')))

def load_game(name):
    spec = importlib.util.spec_from_file_location(f'romset_{name}', os.path.join(GAMES_DIR, name, 'romset.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

# Some sets name the board ROMs with a dash and the IC number.
ALIASES = {'opr14744.bin': 'opr-14744.58', 'opr14745.bin': 'opr-14745.59',
           'opr14746.bin': 'opr-14746.62', 'opr14747.bin': 'opr-14747.63'}

def find(z, name, crc):
    by_crc = {i.CRC: i.filename for i in z.infolist()}
    for n in (name, ALIASES.get(name)):
        if n and n in z.namelist():
            return n
    if crc in by_crc:
        return by_crc[crc]
    sys.exit(f'{name}: not in the zip')

OPTIONAL_IOBOARD = ('epr-14869b.25', 0x2d093304, 'ioboard.bin',
                    'analog inputs need this ROM; digital I/O uses the stand-in')

def find_optional(sources, name, crc):
    for s in sources:
        if s.endswith('.zip'):
            with zipfile.ZipFile(s) as z:
                for i in z.infolist():
                    if i.CRC == crc or os.path.basename(i.filename) == name:
                        d = z.read(i)
                        if zlib.crc32(d) == crc:
                            return d
        elif os.path.isfile(s):
            d = open(s, 'rb').read()
            if zlib.crc32(d) == crc:
                return d
    return None

def main():
    args = sys.argv[1:]
    usage = f'usage: tools/romset.py [--game {"|".join(games())}] rom.zip [output-dir] [extra.zip ...]'
    game = None
    if args[:1] == ['--game']:
        if len(args) < 2 or args[1] not in games():
            sys.exit(usage)
        game, args = args[1], args[2:]
    if not args:
        sys.exit(usage)
    src = args[0]
    if game is None:
        game = os.path.splitext(os.path.basename(src))[0]
        if game not in games():
            sys.exit(f'which game is {src}? say with --game ({", ".join(games())})')
    out = args[1] if len(args) > 1 else f'build/{game}'
    extra = args[2:]
    title = load_game(game)
    roms = BOARD_ROMS + title.ROMS
    regions = {k: bytearray([fill]) * size for k, (size, fill) in {**COMMON_REGIONS, **title.REGIONS}.items()}
    os.makedirs(out, exist_ok=True)
    with zipfile.ZipFile(src) as z:
        for name, crc, region, off, kind in roms:
            data = z.read(find(z, name, crc))
            if zlib.crc32(data) != crc:
                sys.exit(f'{name}: CRC {zlib.crc32(data):08x}, expected {crc:08x}')
            r = regions[region]
            if kind == 'raw':
                r[off:off + len(data)] = data
            elif kind == 'byte16':
                r[off:off + 2 * len(data):2] = data
            elif kind == 'wordswap':
                sw = bytearray(len(data)); sw[0::2] = data[1::2]; sw[1::2] = data[0::2]
                r[off:off + len(data)] = sw
            elif kind == 'word32':
                for i in range(0, len(data), 2):
                    r[off + 2 * i:off + 2 * i + 2] = data[i:i + 2]
            elif kind == 'byte32':
                r[off:off + 4 * len(data):4] = data
    for k, r in regions.items():
        with open(os.path.join(out, k + '.bin'), 'wb') as f:
            f.write(r)
    print(f'{len(roms)} ROMs checked, {len(regions)} regions written to {out}/')
    optional_sources = [src] + extra
    common_ioboard = 'roms/model1io.zip'
    if os.path.isfile(common_ioboard):
        optional_sources.append(common_ioboard)
    for name, crc, dst, missing in title.OPTIONAL + [OPTIONAL_IOBOARD]:
        d = find_optional(optional_sources, name, crc)
        if d:
            with open(os.path.join(out, dst), 'wb') as f:
                f.write(d)
            print(f'{name}: found, written to {out}/{dst}')
        else:
            print(f'{name}: not found ({missing})')

if __name__ == '__main__':
    main()
