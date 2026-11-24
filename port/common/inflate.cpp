#include "inflate.h"
#include <string.h>

namespace Inflate {
namespace {

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;

struct Bits {
	const u8 *p, *end; u32 buf; int cnt; int overrun;
	void need(int n) {
		while (cnt < n) {
			u32 b = 0;
			if (p < end) b = *p++; else overrun++;
			buf |= b << cnt; cnt += 8;
		}
	}
	u32 get(int n) {            /* n <= 16 */
		if (n == 0) return 0;
		need(n);
		u32 v = buf & ((1u << n) - 1);
		buf >>= n; cnt -= n;
		return v;
	}
};

struct Table { u16 e[1 << 15]; int bits; };

/* Table de décodage directe : index = prochains `bits` bits ; entrée = (symbole << 4) | longueur. */
bool build(const u8 *lens, int n, Table &t) {
	int count[16] = { 0 };
	int maxLen = 0;
	for (int i = 0; i < n; i++) { count[lens[i]]++; if (lens[i] > maxLen) maxLen = lens[i]; }
	count[0] = 0;
	if (maxLen == 0) { t.bits = 1; t.e[0] = t.e[1] = 0; return true; }   /* aucun code : tout symbole sera refusé */
	int code = 0, next[16];
	for (int l = 1; l <= 15; l++) { code = (code + count[l - 1]) << 1; next[l] = code; }
	/* vérifie que le code n'est pas sur-souscrit */
	int left = 1;
	for (int l = 1; l <= 15; l++) { left <<= 1; left -= count[l]; if (left < 0) return false; }
	t.bits = maxLen;
	memset(t.e, 0, sizeof(u16) << maxLen);
	for (int s = 0; s < n; s++) {
		int l = lens[s];
		if (!l) continue;
		int c = next[l]++;
		int r = 0;                                   /* code inversé (lu bit de poids faible d'abord) */
		for (int i = 0; i < l; i++) r |= ((c >> i) & 1) << (l - 1 - i);
		for (int i = r; i < (1 << maxLen); i += 1 << l) t.e[i] = (u16)((s << 4) | l);
	}
	return true;
}

inline int decode(Bits &b, const Table &t) {
	b.need(t.bits);
	u16 e = t.e[b.buf & ((1u << t.bits) - 1)];
	int l = e & 15;
	if (!l) return -1;
	b.buf >>= l; b.cnt -= l;
	return e >> 4;
}

const u16 kLenBase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
const u8 kLenExtra[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
const u16 kDistBase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
const u8 kDistExtra[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

Table g_lit, g_dist, g_cl;

}  // namespace

long raw(const u8 *src, size_t srcLen, u8 *dst, size_t dstCap) {
	Bits b = { src, src + srcLen, 0, 0, 0 };
	size_t o = 0;
	int last;
	do {
		last = (int)b.get(1);
		int type = (int)b.get(2);
		if (type == 0) {
			b.buf >>= (b.cnt & 7); b.cnt -= (b.cnt & 7);
			u32 len = b.get(16), nlen = b.get(16);
			if ((len ^ 0xffff) != nlen) return -1;
			if (o + len > dstCap) return -1;
			for (u32 i = 0; i < len; i++) {
				u8 v;
				if (b.cnt >= 8) { v = (u8)(b.buf & 255); b.buf >>= 8; b.cnt -= 8; }
				else { if (b.p >= b.end) return -1; v = *b.p++; }
				dst[o++] = v;
			}
		} else if (type == 1 || type == 2) {
			if (type == 1) {
				u8 l[288], d[30];
				for (int i = 0; i < 144; i++) l[i] = 8;
				for (int i = 144; i < 256; i++) l[i] = 9;
				for (int i = 256; i < 280; i++) l[i] = 7;
				for (int i = 280; i < 288; i++) l[i] = 8;
				for (int i = 0; i < 30; i++) d[i] = 5;
				build(l, 288, g_lit); build(d, 30, g_dist);
			} else {
				int hlit = (int)b.get(5) + 257, hdist = (int)b.get(5) + 1, hclen = (int)b.get(4) + 4;
				static const u8 order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
				u8 cl[19] = { 0 };
				for (int i = 0; i < hclen; i++) cl[order[i]] = (u8)b.get(3);
				if (!build(cl, 19, g_cl)) return -1;
				u8 lens[288 + 32] = { 0 };
				int n = 0;
				while (n < hlit + hdist) {
					int s = decode(b, g_cl);
					if (s < 0) return -1;
					if (s < 16) lens[n++] = (u8)s;
					else {
						int rep, val = 0;
						if (s == 16) { if (!n) return -1; val = lens[n - 1]; rep = 3 + (int)b.get(2); }
						else if (s == 17) rep = 3 + (int)b.get(3);
						else rep = 11 + (int)b.get(7);
						if (n + rep > hlit + hdist) return -1;
						while (rep--) lens[n++] = (u8)val;
					}
				}
				if (!build(lens, hlit, g_lit) || !build(lens + hlit, hdist, g_dist)) return -1;
			}
			for (;;) {
				int s = decode(b, g_lit);
				if (s < 0) return -1;
				if (s < 256) {
					if (o >= dstCap) return -1;
					dst[o++] = (u8)s;
				} else if (s == 256) break;
				else {
					s -= 257;
					if (s >= 29) return -1;
					size_t len = kLenBase[s] + b.get(kLenExtra[s]);
					int ds = decode(b, g_dist);
					if (ds < 0 || ds >= 30) return -1;
					size_t dist = kDistBase[ds] + b.get(kDistExtra[ds]);
					if (dist > o || o + len > dstCap) return -1;
					u8 *d = dst + o; const u8 *sp = d - dist;
					if (dist >= len) memcpy(d, sp, len);
					else for (size_t i = 0; i < len; i++) d[i] = sp[i];
					o += len;
				}
				if (b.overrun > 4) return -1;
			}
		} else return -1;
		if (b.overrun > 4) return -1;
	} while (!last);
	return (long)o;
}

}  // namespace Inflate
