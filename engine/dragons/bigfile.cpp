/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include "dragons/bigfile.h"
#include "bdprof.h"
#include "inflate.h"
#include "disc.h"
#include "prefetch.h"
#include "dragons/dragons.h"

namespace Dragons {

uint32 BigfileArchive::getResourceId(const char *filename) {
	for (uint32 i = 0; i < _totalRecords; i++) {
		if (scumm_stricmp(_fileInfoTbl[i].filename.c_str(), filename) == 0) {
			return i;
		}
	}

	return _totalRecords;
}

BigfileArchive::BigfileArchive(DragonsEngine *vm, const char *filename) :_vm(vm), _disc(nullptr), _packed(false), _packTbl(nullptr) {
	uint32 packCount = 0;
	Disc::File *pk = Disc::open("bigpack.dat");
	if (pk) {
		byte hdr[8];
		if (Disc::readAt(pk, 0, hdr, 8) == 8 && memcmp(hdr, "BDPK", 4) == 0) {
			uint32 n = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | ((uint32)hdr[7] << 24);
			_packTbl = (uint32 *)malloc(n * 16);
			byte *raw = (byte *)malloc(n * 16);
			if (_packTbl && raw && (uint32)Disc::readAt(pk, 8, raw, n * 16) == n * 16) {
				for (uint32 i = 0; i < n * 4; i++) _packTbl[i] = raw[i * 4] | (raw[i * 4 + 1] << 8) | (raw[i * 4 + 2] << 16) | ((uint32)raw[i * 4 + 3] << 24);
				_packed = true; packCount = n;
			}
			free(raw);
		}
		if (_packed) _disc = pk;
		else { Disc::close(pk); free(_packTbl); _packTbl = nullptr; }
	}
	if (!_packed) {
		_disc = Disc::open(filename);
		if (!_disc) error("BigfileArchive::BigfileArchive() Could not open %s", filename);
	}

	_totalRecords = _vm->getBigFileTotalRecords();
	_fileInfoTbl.resize(_totalRecords);

	loadFileInfoTbl();
	if (_packed) Prefetch::configure("bigpack.dat", _packTbl, packCount);
}

BigfileArchive::~BigfileArchive() {
	Prefetch::shutdown();
	free(_packTbl);
	Disc::close(_disc);
}

void BigfileArchive::loadFileInfoTbl() {
	char filename[16];
	Common::File fd;
	if (!fd.open("dragon.exe")) {
		error("Failed to open dragon.exe");
	}

	fd.seek(_vm->getBigFileInfoTblFromDragonEXE());

	for (int i = 0; i < _totalRecords; i++) {
		fd.read(filename, 16);
		filename[15] = 0;
		_fileInfoTbl[i].filename = filename;
		_fileInfoTbl[i].offset = fd.readUint32LE() * 2048;
		_fileInfoTbl[i].size = fd.readUint32LE();
		fd.skip(4);
	}
}

/* Port DC : cache en mémoire des fichiers déjà lus. Le jeu recharge sans cesse les mêmes ressources
   (cursor.act 56 fois, etc.) et chaque lecture coûte une seconde ou plus sur le disque.
   L'appelant devient propriétaire du tampon renvoyé (il le libère), on renvoie donc une copie. */
namespace {
struct CacheEntry { int id; byte *data; uint32 size; uint32 stamp; };
const int kCacheSlots = 40;
const uint32 kCacheBudget = 1536 * 1024;     /* octets de données conservés */
const uint32 kCacheMaxFile = 300 * 1024;
CacheEntry g_cache[kCacheSlots];
uint32 g_cacheBytes = 0, g_cacheClock = 0;
}

static void cacheEvictOne() {
	int victim = -1;
	for (int i = 0; i < kCacheSlots; i++)
		if (g_cache[i].data && (victim < 0 || g_cache[i].stamp < g_cache[victim].stamp)) victim = i;
	if (victim < 0) return;
	g_cacheBytes -= g_cache[victim].size;
	free(g_cache[victim].data);
	g_cache[victim].data = nullptr;
}

byte *BigfileArchive::load(const char *filename, uint32 &dataSize) {
	uint32 id = getResourceId(filename);
	if (id >= _totalRecords) {
		error("Invalid resourceID for input filename: %s", filename);
	}
	dataSize = _fileInfoTbl[id].size;

	for (int i = 0; i < kCacheSlots; i++) {
		if (g_cache[i].data && g_cache[i].id == (int)id) {
			byte *copy = (byte *)malloc(dataSize ? dataSize : 1);
			if (copy) {
				memcpy(copy, g_cache[i].data, dataSize);
				g_cache[i].stamp = ++g_cacheClock;
				BdProf::logLoad(filename, dataSize, 0, true);   /* servi par la mémoire */
				return copy;
			}
			break;
		}
	}

	unsigned long long t0 = BdProf::nowFn ? BdProf::nowFn() : 0;
	byte *buf = (byte *)malloc(dataSize ? dataSize : 1);
	if (!buf) { Prefetch::flush(); buf = (byte *)malloc(dataSize ? dataSize : 1); }
	if (!buf) {
		error("Failed to malloc %d bytes for '%s'", dataSize, filename);
	}
	uint32 diskBytes = dataSize;
	bool fromPrefetch = false;
	if (_packed) {
		const uint32 *r = _packTbl + id * 4;     /* offset, compressé, origine, méthode */
		diskBytes = r[1];
		if (r[2] != dataSize) error("BIGPACK.DAT incoherent pour '%s'", filename);
		uint32 pcs = 0;
		byte *z = Prefetch::take(id, &pcs);      /* déjà lu en arrière-plan ? */
		if (z) {
			fromPrefetch = true; diskBytes = 0;
		} else {
			BdProf::Scope prof(BdProf::DISC);
			if (r[3] == 0) {
				Disc::readAt(_disc, r[0], buf, dataSize);
			} else {
				z = (byte *)malloc(r[1] ? r[1] : 1);
				if (!z) { Prefetch::flush(); z = (byte *)malloc(r[1] ? r[1] : 1); }
				if (!z) error("Failed to malloc %d bytes for '%s'", r[1], filename);
				Disc::readAt(_disc, r[0], z, r[1]);
			}
			BdProf::discBytes += r[1];
		}
		if (r[3] == 0) {
			if (z) { memcpy(buf, z, dataSize); free(z); }
		} else {
			long n = Inflate::raw(z, r[1], buf, dataSize);
			free(z);
			if (n != (long)dataSize) error("Decompression echouee pour '%s'", filename);
		}
	} else {
		BdProf::Scope prof(BdProf::DISC);
		Disc::readAt(_disc, _fileInfoTbl[id].offset, buf, dataSize);
		BdProf::discBytes += dataSize;
	}
	BdProf::logLoad(filename, diskBytes, BdProf::nowFn ? (unsigned)(BdProf::nowFn() - t0) : 0, fromPrefetch);

	if (dataSize <= kCacheMaxFile) {
		while (g_cacheBytes + dataSize > kCacheBudget) { uint32 before = g_cacheBytes; cacheEvictOne(); if (g_cacheBytes == before) break; }
		if (g_cacheBytes + dataSize <= kCacheBudget) {
			for (int i = 0; i < kCacheSlots; i++) {
				if (!g_cache[i].data) {
					byte *keep = (byte *)malloc(dataSize ? dataSize : 1);
					if (keep) {
						memcpy(keep, buf, dataSize);
						g_cache[i].id = id; g_cache[i].data = keep; g_cache[i].size = dataSize; g_cache[i].stamp = ++g_cacheClock;
						g_cacheBytes += dataSize;
					}
					break;
				}
			}
		}
	}
	return buf;
}

bool BigfileArchive::isCached(uint32 id) {
	for (int i = 0; i < kCacheSlots; i++)
		if (g_cache[i].data && g_cache[i].id == (int)id) return true;
	return false;
}

bool BigfileArchive::doesFileExist(const char *filename) {
	uint32 id = getResourceId(filename);
	return (id < _totalRecords);
}


} // End of namespace Dragons
