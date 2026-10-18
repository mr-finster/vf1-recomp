"""Virtua Fighter's ROM set for tools/romset.py: MAME's src/mame/sega/model1.cpp, ROM_START(vf),
past the CPU board's ROMs (tools/romset.py has those). (file, crc, region, offset, kind)"""

ROMS = [
    ('epr-16082.14', 0xb23f22ee, 'maincpu', 0x200000, 'byte16'),
    ('epr-16083.15', 0xd12c77f8, 'maincpu', 0x200001, 'byte16'),
    ('epr-16080.4',  0x3662e1a5, 'maincpu', 0xfc0000, 'raw'),
    ('epr-16081.5',  0x6dec06ce, 'maincpu', 0xfe0000, 'raw'),
    ('mpr-16084.6',  0x483f453b, 'maincpu', 0x1000000, 'byte16'),
    ('mpr-16085.7',  0x5fa01277, 'maincpu', 0x1000001, 'byte16'),
    ('mpr-16086.8',  0xdeac47a1, 'maincpu', 0x1100000, 'byte16'),
    ('mpr-16087.9',  0x7a64daac, 'maincpu', 0x1100001, 'byte16'),
    ('mpr-16088.10', 0xfcda2d1e, 'maincpu', 0x1200000, 'byte16'),
    ('mpr-16089.11', 0x39befbe0, 'maincpu', 0x1200001, 'byte16'),
    ('mpr-16090.12', 0x90c76831, 'maincpu', 0x1300000, 'byte16'),
    ('mpr-16091.13', 0x53115448, 'maincpu', 0x1300001, 'byte16'),
    ('epr-16120.7',  0x2bff8378, 'sndcpu', 0x00000, 'wordswap'),
    ('epr-16121.8',  0xff6723f9, 'sndcpu', 0x20000, 'wordswap'),
    ('epr-16121.8',  0xff6723f9, 'sndcpu', 0x80000, 'wordswap'),   # ROM_RELOAD
    ('mpr-16122.32', 0x568bc64e, 'pcm1', 0x000000, 'raw'),
    ('mpr-16123.33', 0x15d78844, 'pcm1', 0x200000, 'raw'),
    ('mpr-16124.4',  0x45520ba1, 'pcm2', 0x000000, 'raw'),
    ('mpr-16125.5',  0x9b4998b6, 'pcm2', 0x200000, 'raw'),
    ('mpr-16096.26', 0xa92b0bf3, 'polygons', 0x000000, 'word32'),
    ('mpr-16097.27', 0x0232955a, 'polygons', 0x000002, 'word32'),
    ('mpr-16098.28', 0xcf2e1b84, 'polygons', 0x400000, 'word32'),
    ('mpr-16099.29', 0x20e46854, 'polygons', 0x400002, 'word32'),
    ('mpr-16100.30', 0xe13e983d, 'polygons', 0x800000, 'word32'),
    ('mpr-16101.31', 0x0dbed94d, 'polygons', 0x800002, 'word32'),
    ('mpr-16102.32', 0x4cb41fb6, 'polygons', 0xc00000, 'word32'),
    ('mpr-16103.33', 0x526d1c76, 'polygons', 0xc00002, 'word32'),
]

# the regions past the Model 1 board's own: (size, fill)
REGIONS = {}

# files written when found: (file, crc, output, what happens without it). From a current MAME set:
# the TGP's own program (the runtime then runs the real TGP code)
OPTIONAL = [('315-5724.bin', 0x4b4f330e, 'tgp_program.bin', 'using the simulated TGP')]
