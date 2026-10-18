"""Virtua Racing's ROM set for tools/romset.py: MAME's src/mame/sega/model1.cpp, ROM_START(vr),
past the CPU board's ROMs (tools/romset.py has those). (file, crc, region, offset, kind)"""

ROMS = [
    ('epr-14882.14', 0x547d75ad, 'maincpu', 0x200000, 'byte16'),
    ('epr-14883.15', 0x6bfad8b1, 'maincpu', 0x200001, 'byte16'),
    ('epr-14878a.4', 0x6d69e695, 'maincpu', 0xfc0000, 'raw'),
    ('epr-14879a.5', 0xd45af9dd, 'maincpu', 0xfe0000, 'raw'),
    ('mpr-14880.6', 0xadc7c208, 'maincpu', 0x1000000, 'byte16'),
    ('mpr-14881.7', 0xe5ab89df, 'maincpu', 0x1000001, 'byte16'),
    ('mpr-14884.8', 0x6cf9c026, 'maincpu', 0x1100000, 'byte16'),
    ('mpr-14885.9', 0xf65c9262, 'maincpu', 0x1100001, 'byte16'),
    ('mpr-14886.10', 0x92868734, 'maincpu', 0x1200000, 'byte16'),
    ('mpr-14887.11', 0x10c7c636, 'maincpu', 0x1200001, 'byte16'),
    ('mpr-14888.12', 0x04bfdc5b, 'maincpu', 0x1300000, 'byte16'),
    ('mpr-14889.13', 0xc49f0486, 'maincpu', 0x1300001, 'byte16'),
    ('315-5573.bin', 0x3335a19b, 'tgp_program', 0x0, 'raw'),
    ('epr-14870a.7', 0x919d9b75, 'sndcpu', 0x0, 'wordswap'),
    ('mpr-14873.32', 0xb1965190, 'pcm1', 0x0, 'raw'),
    ('mpr-14876.4', 0xba6b2327, 'pcm2', 0x0, 'raw'),
    ('mpr-14890.26', 0xdcbe006b, 'polygons', 0x000000, 'word32'),
    ('mpr-14891.27', 0x25832b38, 'polygons', 0x000002, 'word32'),
    ('mpr-14892.28', 0x5136f3ba, 'polygons', 0x400000, 'word32'),
    ('mpr-14893.29', 0x1c531ada, 'polygons', 0x400002, 'word32'),
    ('mpr-14894.30', 0x830a71bc, 'polygons', 0x800000, 'word32'),
    ('mpr-14895.31', 0xaf027ac5, 'polygons', 0x800002, 'word32'),
    ('mpr-14896.32', 0x382091dc, 'polygons', 0xc00000, 'word32'),
    ('mpr-14897.33', 0x74873195, 'polygons', 0xc00002, 'word32'),
    # the TGP's track data
    ('mpr-14898.39', 0x61da2bb6, 'copro_data', 0x000000, 'byte32'),
    ('mpr-14899.40', 0x2cd58bee, 'copro_data', 0x000001, 'byte32'),
    ('mpr-14900.41', 0xaa7c017d, 'copro_data', 0x000002, 'byte32'),
    ('mpr-14901.42', 0x175b7a9a, 'copro_data', 0x000003, 'byte32'),
]

# the regions past the Model 1 board's own: (size, fill)
REGIONS = {
    'copro_data': (0x200000, 0x00),
    'tgp_program': (0x2000, 0x00),
}

# files written when found: (file, crc, output, what happens without it)
OPTIONAL = []
