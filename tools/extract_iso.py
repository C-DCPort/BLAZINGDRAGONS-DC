#!/usr/bin/env python3
"""Extrait les fichiers d'une image CD PlayStation (BIN MODE2/2352, piste unique).

Usage : extract_iso.py image.bin dossier_sortie

- Fichiers normaux (Mode 2 Form 1) : 2048 octets utiles par secteur.
- Fichiers .STR / .XA (vidéo / voix, souvent en Form 2) : copiés en secteurs
  bruts de 2352 octets, comme le demande ScummVM (HOWTO-PlayStation_Videos).
Écrit aussi un manifeste avec taille, LBA et MD5 de chaque fichier.
"""
import hashlib, os, struct, sys

RAW = 2352

def sector(f, lba):
    f.seek(lba * RAW)
    return f.read(RAW)

def data(f, lba):
    return sector(f, lba)[24:24 + 2048]

def read_extent(f, lba, size):
    out = bytearray()
    n = (size + 2047) // 2048
    for i in range(n):
        out += data(f, lba + i)
    return bytes(out[:size])

def walk(f, lba, size, path, entries):
    buf = read_extent(f, lba, size)
    pos = 0
    while pos < len(buf):
        ln = buf[pos]
        if ln == 0:  # fin du secteur courant
            pos = (pos // 2048 + 1) * 2048
            continue
        rec = buf[pos:pos + ln]
        elba = struct.unpack_from('<I', rec, 2)[0]
        esize = struct.unpack_from('<I', rec, 10)[0]
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33 + nlen]
        pos += ln
        if name in (b'\x00', b'\x01'):
            continue
        name = name.decode('ascii', 'replace').split(';')[0]
        full = os.path.join(path, name)
        if flags & 2:
            walk(f, elba, esize, full, entries)
        else:
            entries.append((full, elba, esize))

def main(img, outdir):
    with open(img, 'rb') as f:
        pvd = data(f, 16)
        assert pvd[1:6] == b'CD001', 'pas un ISO9660'
        root = pvd[156:156 + 34]
        rlba = struct.unpack_from('<I', root, 2)[0]
        rsize = struct.unpack_from('<I', root, 10)[0]
        entries = []
        walk(f, rlba, rsize, '', entries)
        man = []
        for name, lba, size in entries:
            dst = os.path.join(outdir, name)
            os.makedirs(os.path.dirname(dst) or outdir, exist_ok=True)
            raw = name.upper().endswith(('.STR', '.XA'))
            h = hashlib.md5()
            with open(dst, 'wb') as o:
                n = (size + 2047) // 2048
                for i in range(n):
                    if raw:
                        blk = sector(f, lba + i)
                    else:
                        blk = data(f, lba + i)
                        if i == n - 1 and size % 2048:
                            blk = blk[:size % 2048]
                    o.write(blk)
                    h.update(blk)
            man.append(f'{h.hexdigest()}  {lba:7d}  {os.path.getsize(dst):10d}  {"RAW " if raw else "    "}{name}')
        with open(os.path.join(outdir, 'MANIFEST.txt'), 'w') as m:
            m.write('\n'.join(man) + '\n')
        print('\n'.join(man))

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
