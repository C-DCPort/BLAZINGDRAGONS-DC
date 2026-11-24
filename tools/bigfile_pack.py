#!/usr/bin/env python3
"""BIGFILE.DAT -> BIGPACK.DAT : chaque ressource compressée (deflate) séparément.

Les données du jeu se compressent environ 3 fois (acteurs 4 à 8 fois) ; sur le disque, lire moins
d'octets réduit d'autant les temps de chargement.
usage : bigfile_pack.py DRAGON.EXE BIGFILE.DAT BIGPACK.DAT [--table 0x4b250] [--n 588]
  --table : offset de l'annuaire dans DRAGON.EXE (FR 0x4b250, US 0x4a238, GB 0x4b5f4, DE 0x4b054)
Format : 'BDPK', n (u32), n x { offset u32, taille compressée u32, taille d'origine u32, méthode u32 (0 brut, 1 deflate) },
         puis les données. L'index est celui de l'annuaire de DRAGON.EXE.
"""
import struct, sys, zlib

def opt(name, default):
    return int(sys.argv[sys.argv.index(name) + 1], 0) if name in sys.argv else default

def main():
    pos = [a for i, a in enumerate(sys.argv[1:], 1) if not a.startswith('--') and not (sys.argv[i - 1].startswith('--'))]
    exe_path, big_path, out_path = pos[:3]
    table, n = opt('--table', 0x4b250), opt('--n', 588)
    exe = open(exe_path, 'rb').read()
    big = open(big_path, 'rb').read()
    recs, blob = [], bytearray()
    hdr = 8 + 16 * n
    tot_in = tot_out = 0
    for i in range(n):
        e = exe[table + i * 28: table + i * 28 + 28]
        sec, size = struct.unpack('<II', e[16:24])
        off = sec * 2048
        data = big[off:off + size] if off + size <= len(big) else b''
        if len(data) != size:
            recs.append((0, 0, 0, 0)); continue
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        z = c.compress(data) + c.flush()
        if len(z) < size:
            method, payload = 1, z
        else:
            method, payload = 0, data
        recs.append((hdr + len(blob), len(payload), size, method))
        blob += payload
        tot_in += size; tot_out += len(payload)
    with open(out_path, 'wb') as f:
        f.write(b'BDPK' + struct.pack('<I', n))
        for r in recs: f.write(struct.pack('<IIII', *r))
        f.write(blob)
    print('BIGPACK.DAT : %.1f Mo -> %.1f Mo (%.0f %%)' % (tot_in / 1048576, tot_out / 1048576, 100.0 * tot_out / max(tot_in, 1)))

main()
