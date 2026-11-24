#!/usr/bin/env python3
"""Reorganise DTSPEECH.XA en SPEECH.PAK : une réplique = secteurs ADPCM contigus.

Le fichier d'origine est entrelacé (1 secteur utile tous les 32) : le lire tel quel sur disque
demande un déplacement de la tête par secteur. Le .PAK range chaque réplique d'un seul tenant.

usage : xa_pack.py DTSPEECH.XA DRAGON.EXE SPEECH.PAK [--table 0x4f2a0]
  --table : offset de la table des répliques dans DRAGON.EXE (français : 0x4f2a0, US : 0x4e138,
            GB : 0x4f4f4, DE : 0x4f0a4).
Format : 'SPCH', n (u32), n x { id u32, offset u32, secteurs u16, format u8, 0 } triés par id,
         puis les données (2304 octets par secteur).
"""
import struct, sys

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    table = 0x4f2a0
    if '--table' in sys.argv:
        table = int(sys.argv[sys.argv.index('--table') + 1], 0)
        args = [a for a in args if a != sys.argv[sys.argv.index('--table') + 1]]
    xa_path, exe_path, out_path = args[:3]
    exe = open(exe_path, 'rb').read()
    xa = open(xa_path, 'rb').read()
    nsec_file = len(xa) // 2352
    entries = []
    for i in range(2272):
        o = table + i * 8
        idw, start, end = struct.unpack_from('<IHH', exe, o)[0], *struct.unpack_from('<HH', exe, o + 4)
        ident = idw & 0xffffff
        start_off = struct.unpack_from('<b', exe, o + 3)[0]
        entries.append((ident, start_off, start, end))
    clips = {}     # (start, off, end) -> (offset, nsec, fmt)
    data = bytearray()
    out = []
    skipped = 0
    for ident, so, start, end in entries:
        key = (start, so, end)
        if key not in clips:
            n = end - start
            first = start * 32 + so
            if n <= 0 or first + (n - 1) * 32 >= nsec_file:
                clips[key] = None
            else:
                fmt = xa[first * 2352 + 19]
                off = len(data)
                for k in range(n):
                    s = (first + k * 32) * 2352
                    data += xa[s + 24:s + 24 + 2304]
                clips[key] = (off, n, fmt)
        c = clips[key]
        if c is None:
            skipped += 1
            continue
        out.append((ident, c[0], c[1], c[2]))
    out.sort()
    hdr_size = 8 + 12 * len(out)
    with open(out_path, 'wb') as f:
        f.write(b'SPCH' + struct.pack('<I', len(out)))
        for ident, off, n, fmt in out:
            f.write(struct.pack('<IIHBB', ident, hdr_size + off, n, fmt, 0))
        f.write(data)
    print('SPEECH.PAK : %d repliques, %d ignorees, %.1f Mo' % (len(out), skipped, (hdr_size + len(data)) / 1048576))

main()
