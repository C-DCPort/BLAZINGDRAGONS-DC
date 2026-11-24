#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "hd_pack.h"
#include "disc.h"
#include <string.h>

namespace Gpu {

HdPack::HdPack() : _f(nullptr) {}

HdPack::~HdPack() { if (_f) Disc::close(_f); }

bool HdPack::open(const char *name) {
	if (_f) { Disc::close(_f); _f = nullptr; }
	_entries.clear();
	_f = Disc::open(name, true);   /* tampon de 32 Ko (la mémoire est comptée) */
	if (!_f) return false;
	byte head[16];
	if (Disc::readAt(_f, 0, head, 16) != 16 || memcmp(head, "BDHD", 4) != 0) { Disc::close(_f); _f = nullptr; return false; }
	const int version = head[4] | (head[5] << 8);
	const int n = head[6] | (head[7] << 8);
	if (version != 1 || n <= 0 || n > 4096) { Disc::close(_f); _f = nullptr; return false; }
	std::vector<byte> tab((size_t)n * 32);
	if (Disc::readAt(_f, 16, tab.data(), (uint32)tab.size()) != (long)tab.size()) { Disc::close(_f); _f = nullptr; return false; }
	for (int i = 0; i < n; i++) {
		const byte *p = &tab[(size_t)i * 32];
		HdEntry e;
		memset(&e, 0, sizeof(e));
		memcpy(e.scene, p, 8);
		e.layer = p[8]; e.fmt = p[9]; e.tilesX = p[10]; e.tilesY = p[11];
		e.w = p[12] | (p[13] << 8); e.h = p[14] | (p[15] << 8);
		e.oh = p[24] | (p[25] << 8); if (e.oh == 0) e.oh = e.h / 2;   /* octets réservés : 0 = ancien format, x2 */
		e.offset = p[16] | (p[17] << 8) | (p[18] << 16) | ((uint32)p[19] << 24);
		e.size = p[20] | (p[21] << 8) | (p[22] << 16) | ((uint32)p[23] << 24);
		if (e.tilesX == 0 || e.tilesY == 0 || e.size != (uint32)e.tilesX * e.tilesY * HD_TILE_BYTES) continue;
		_entries.push_back(e);
	}
	return !_entries.empty();
}

const HdEntry *HdPack::find(const char *scene, int layer) const {
	for (size_t i = 0; i < _entries.size(); i++)
		if (_entries[i].layer == layer && strncmp(_entries[i].scene, scene, 8) == 0) return &_entries[i];
	return nullptr;
}

bool HdPack::readTile(const HdEntry &e, int i, void *dst) {
	if (!_f) return false;
	return Disc::readAt(_f, e.offset + (uint32)i * HD_TILE_BYTES, dst, HD_TILE_BYTES) == (long)HD_TILE_BYTES;
}

bool HdPack::read(const HdEntry &e, void *dst) {
	if (!_f) return false;
	return Disc::readAt(_f, e.offset, dst, e.size) == (long)e.size;
}


HdSprPack::HdSprPack() : _f(nullptr) {}
HdSprPack::~HdSprPack() {
	for (Cached *c : _cache) delete c;
	if (_f) Disc::close(_f);
}

static uint32 rd32(const byte *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32)p[3] << 24); }
static uint16 rd16(const byte *p) { return p[0] | (p[1] << 8); }

#ifndef HD_SPR_INDEX_BUDGET
#define HD_SPR_INDEX_BUDGET (64 * 1024)
#endif

bool HdSprPack::open(const char *name) {
	if (_f) { Disc::close(_f); _f = nullptr; }
	for (Cached *c : _cache) delete c;
	_cache.clear(); _groups.clear(); _pageFmt.clear(); _cacheBytes = 0; _nFrames = 0; _nPages = 0;
	_f = Disc::open(name, true);   /* tampon de 32 Ko (la mémoire est comptée) */
	if (!_f) return false;
	#define BAD do { Disc::close(_f); _f = nullptr; _groups.clear(); _pageFmt.clear(); return false; } while (0)
	byte head[32];
	if (Disc::readAt(_f, 0, head, 32) != 32 || memcmp(head, "BDHS", 4) != 0) BAD;
	_version = rd16(head + 4);
	if (_version != 2 && _version != 3) BAD;   /* v1 : index entier en RAM, refusé */
	const uint32 nf = rd32(head + 8), npg = rd32(head + 16), ng = rd32(head + 20);
	_dataStart = rd32(head + 24);
	/* v3 : pages de côté donné (64) et format de chaque page dans les 2 bits hauts du numéro de page des morceaux (pas de table en RAM) */
	_pageSize = _version >= 3 ? rd16(head + 28) : 256;
	if (_pageSize < 8 || _pageSize > 256 || (_pageSize & (_pageSize - 1))) BAD;
	_pageBytes = 2048 + (uint32)(_pageSize / 2) * (_pageSize / 2);
	_nPages = npg;
	if (nf == 0 || ng == 0 || ng > 4096 || npg > (1u << 24)) BAD;
	const uint32 fmtBytes = _version >= 3 ? 0 : (npg + 3) / 4 * 4;
	std::vector<byte> tab((size_t)ng * 16 + fmtBytes);
	if (Disc::readAt(_f, 32, tab.data(), (uint32)tab.size()) != (long)tab.size()) BAD;
	const byte *p = tab.data();
	for (uint32 i = 0; i < ng; i++, p += 16) {
		Group g = { rd16(p), rd16(p + 2), rd16(p + 4), rd32(p + 8), rd32(p + 12) };
		if (g.size < (uint32)g.nFrames * 10 + (uint32)g.nPieces * 16 || g.size > 256 * 1024) BAD;
		_groups.push_back(g);
	}
	if (_version < 3) _pageFmt.assign(p, p + npg);
	#undef BAD
	_nFrames = nf;
	return true;
}

HdSprPack::Cached *HdSprPack::loadGroup(const Group &g) {
	std::vector<byte> b(g.size);
	if (Disc::readAt(_f, g.off, b.data(), g.size) != (long)g.size) return nullptr;
	Cached *c = new Cached;
	c->id = g.id; c->lastUse = _use;
	c->frames.resize(g.nFrames); c->pieces.resize(g.nPieces);
	const byte *p = b.data();
	for (uint32 i = 0; i < g.nPieces; i++) {
		const byte *q = p + (size_t)g.nFrames * 10 + (size_t)i * 16;
		HdSprPiece &s = c->pieces[i];
		s.page = rd32(q); s.x = rd16(q + 4); s.y = rd16(q + 6); s.w = rd16(q + 8); s.h = rd16(q + 10); s.dx = rd16(q + 12); s.dy = rd16(q + 14);
		if (_version >= 3) { s.fmt = (uint8)(s.page >> 30); s.page &= 0x3fffffff; }
		if (s.page >= _nPages) { delete c; return nullptr; }
		if (_version < 3) s.fmt = _pageFmt[s.page];
	}
	for (uint32 i = 0; i < g.nFrames; i++, p += 10) {
		HdSprFrame &f = c->frames[i];
		f.keyLo = rd16(p); f.firstPiece = rd16(p + 2); f.nPieces = p[4]; f.w = rd16(p + 6); f.h = rd16(p + 8);
		f.pieces = c->pieces.data();
		if ((uint32)f.firstPiece + f.nPieces > g.nPieces) { delete c; return nullptr; }
	}
	c->bytes = (uint32)(g.nFrames * sizeof(HdSprFrame) + g.nPieces * sizeof(HdSprPiece) + sizeof(Cached));
	_loads++;
	return c;
}

const HdSprFrame *HdSprPack::find(int actorFile, uint16 frameOffset, int kind) {
	if (!_f) return nullptr;
	const uint16 id = (uint16)(((kind & 3) << 14) | (actorFile & 0x3fff));
	_use++;
	Cached *c = nullptr;
	for (Cached *k : _cache) if (k->id == id) { c = k; break; }
	if (!c) {
		size_t lo = 0, hi = _groups.size();
		while (lo < hi) { size_t mid = (lo + hi) / 2; if (_groups[mid].id < id) lo = mid + 1; else hi = mid; }
		if (lo >= _groups.size() || _groups[lo].id != id) return nullptr;
		c = loadGroup(_groups[lo]);
		if (!c) return nullptr;
		/* Place libre : on retire les groupes les moins récemment utilisés (jamais celui qu'on vient de charger). */
		while (!_cache.empty() && _cacheBytes + c->bytes > HD_SPR_INDEX_BUDGET) {
			size_t old = 0;
			for (size_t i = 1; i < _cache.size(); i++) if (_cache[i]->lastUse < _cache[old]->lastUse) old = i;
			_cacheBytes -= _cache[old]->bytes; delete _cache[old]; _cache.erase(_cache.begin() + old);
		}
		_cache.push_back(c); _cacheBytes += c->bytes;
	}
	c->lastUse = _use;
	size_t lo = 0, hi = c->frames.size();
	while (lo < hi) { size_t mid = (lo + hi) / 2; if (c->frames[mid].keyLo < frameOffset) lo = mid + 1; else hi = mid; }
	return (lo < c->frames.size() && c->frames[lo].keyLo == frameOffset) ? &c->frames[lo] : nullptr;
}

bool HdSprPack::readPage(uint32 page, void *dst) {
	if (!_f || page >= _nPages) return false;
	return Disc::readAt(_f, _dataStart + page * _pageBytes, dst, _pageBytes) == (long)_pageBytes;
}

bool HdSprPack::readPages(uint32 page, int n, void *dst) {
	if (!_f || n < 1 || page + n > _nPages) return false;
	return Disc::readAt(_f, _dataStart + page * _pageBytes, dst, (uint32)n * _pageBytes) == (long)((uint32)n * _pageBytes);
}

} // namespace Gpu
