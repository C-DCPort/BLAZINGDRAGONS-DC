#include "strdec.h"
#include "disc.h"
#include <stdlib.h>
#include <string.h>

namespace Str {

#include "str_tables.inc"

static const int SECTOR = 2352;
static const int HDR = 56;                  /* en-tête de secteur vidéo (24 + 32) */
static const int CHUNK = 2016;              /* octets de flux par secteur vidéo */

static const uint8_t kZigZag[64] = {
	 0,  1,  5,  6, 14, 15, 27, 28,
	 2,  4,  7, 13, 16, 26, 29, 42,
	 3,  8, 12, 17, 25, 30, 41, 43,
	 9, 11, 18, 24, 31, 40, 44, 53,
	10, 19, 23, 32, 39, 45, 52, 54,
	20, 22, 33, 38, 46, 51, 55, 60,
	21, 34, 37, 47, 50, 56, 59, 61,
	35, 36, 48, 49, 57, 58, 62, 63
};
static const uint8_t kQuant[64] = {
	 2, 16, 19, 22, 26, 27, 29, 34,
	16, 16, 22, 24, 27, 29, 34, 37,
	19, 22, 26, 27, 29, 34, 34, 38,
	22, 22, 26, 27, 29, 34, 37, 40,
	22, 26, 27, 29, 32, 35, 40, 48,
	26, 27, 29, 32, 35, 40, 48, 58,
	26, 27, 29, 34, 38, 46, 56, 69,
	27, 29, 35, 38, 46, 56, 69, 83
};

/* M[x][u] = cos((2x+1) u pi/16) / 2, divisé par racine de 2 pour u = 0 */
static const float kM[8][8] = {
	{ 0.353553390593274f,  0.490392640201615f,  0.461939766255643f,  0.415734806151273f,  0.353553390593274f,  0.277785116509801f,  0.191341716182545f,  0.097545161008064f },
	{ 0.353553390593274f,  0.415734806151273f,  0.191341716182545f, -0.097545161008064f, -0.353553390593274f, -0.490392640201615f, -0.461939766255643f, -0.277785116509801f },
	{ 0.353553390593274f,  0.277785116509801f, -0.191341716182545f, -0.490392640201615f, -0.353553390593274f,  0.097545161008064f,  0.461939766255643f,  0.415734806151273f },
	{ 0.353553390593274f,  0.097545161008064f, -0.461939766255643f, -0.277785116509801f,  0.353553390593274f,  0.415734806151273f, -0.191341716182545f, -0.490392640201615f },
	{ 0.353553390593274f, -0.097545161008064f, -0.461939766255643f,  0.277785116509801f,  0.353553390593274f, -0.415734806151273f, -0.191341716182545f,  0.490392640201615f },
	{ 0.353553390593274f, -0.277785116509801f, -0.191341716182545f,  0.490392640201615f, -0.353553390593274f, -0.097545161008064f,  0.461939766255643f, -0.415734806151273f },
	{ 0.353553390593274f, -0.415734806151273f,  0.191341716182545f,  0.097545161008064f, -0.353553390593274f,  0.490392640201615f, -0.461939766255643f,  0.277785116509801f },
	{ 0.353553390593274f, -0.490392640201615f,  0.461939766255643f, -0.415734806151273f,  0.353553390593274f, -0.277785116509801f,  0.191341716182545f, -0.097545161008064f }
};

/* ---- lecteur de bits : mots de 16 bits petit-boutiste, bit de poids fort en premier ----
   (le tampon de l'image est aligné et suivi de zéros de garde : pas de test de limite) */
typedef uint16_t __attribute__((__may_alias__)) u16a;
struct Bits {
	const u16a *w; uint32_t nwords; uint32_t pos;     /* pos en bits */
	bool eos() const { return pos >= nwords * 16; }
	uint32_t peek16() const {
		uint32_t i = pos >> 4;
		uint32_t v = ((uint32_t)w[i] << 16) | w[i + 1];
		return ((v << (pos & 15)) >> 16) & 0xffff;
	}
	void skip(int n) { pos += n; }
	uint32_t get(int n) { uint32_t v = peek16() >> (16 - n); pos += n; return v; }   /* n <= 16 */
};

Decoder::Decoder() : _file(nullptr), _size(0), _pos(0), _buf(nullptr), _bufStart(0), _bufLen(0), _frame(nullptr), _frameCap(0),
	_frameLen(0), _frameChunks(0), _frameNext(0), _frameNo(0), _w(0), _h(0), _sectors(0), _cb(nullptr), _user(nullptr),
	_videoChannel(-1), _vlc(nullptr), _smooth(true), _rowCr(nullptr), _rowCb(nullptr), _y(nullptr), _cb2(nullptr), _cr(nullptr), _mbW(0), _mbH(0) {}

Decoder::~Decoder() { close(); }

bool Decoder::open(const char *name) {
	close();
	_file = Disc::open(name);
	if (!_file) return false;
	_size = Disc::size(_file);
	_buf = (uint8_t *)malloc(16 * SECTOR);
	if (!_buf) { close(); return false; }
	/* table de décodage AC : les 16 bits suivants -> (longueur << 8) | indice du code */
	_vlc = (uint16_t *)malloc(65536 * 2);
	if (!_vlc) { close(); return false; }
	memset(_vlc, 0, 65536 * 2);
	memset(_vlc8, 0, sizeof(_vlc8));
	for (int i = 0; i < 113; i++) {
		int len = kStrAcLen[i];
		uint32_t base = (uint32_t)kStrAcCode[i] << (16 - len);
		uint32_t cnt = 1u << (16 - len);
		for (uint32_t k = 0; k < cnt; k++) _vlc[base + k] = (uint16_t)((len << 8) | (i + 1));
		if (len <= 8) {                         /* table courte (tient dans le cache de données) pour les codes fréquents */
			uint32_t b8 = (uint32_t)kStrAcCode[i] << (8 - len);
			for (uint32_t k = 0; k < (1u << (8 - len)); k++) _vlc8[b8 + k] = (uint16_t)((len << 8) | (i + 1));
		}
	}
	_pos = 0; _bufStart = 0; _bufLen = 0; _frameNo = 0; _sectors = 0; _videoChannel = -1; _w = _h = 0;
	_frameChunks = 0; _frameNext = 0; _frameLen = 0;
	return true;
}

void Decoder::close() {
	if (_file) { Disc::close(_file); _file = nullptr; }
	free(_buf); _buf = nullptr; free(_frame); _frame = nullptr; _frameCap = 0;
	free(_vlc); _vlc = nullptr;
	free(_y); free(_cb2); free(_cr); _y = _cb2 = _cr = nullptr;
	free(_rowCr); free(_rowCb); _rowCr = _rowCb = nullptr; _mbW = _mbH = 0;
}

/* Renvoie le secteur suivant (2352 octets) ou false à la fin du fichier. */
bool Decoder::readSector(const uint8_t **s) {
	if (_pos + SECTOR > (uint32_t)_size) return false;
	if (_pos < _bufStart || _pos + SECTOR > _bufStart + _bufLen) {
		uint32_t want = 16 * SECTOR;
		if (_pos + want > (uint32_t)_size) want = (uint32_t)_size - _pos;
		long r = Disc::readAt(_file, _pos, _buf, want);
		if (r < SECTOR) return false;
		_bufStart = _pos; _bufLen = (uint32_t)r;
	}
	*s = _buf + (_pos - _bufStart);
	_pos += SECTOR;
	_sectors++;
	return true;
}

static const uint8_t kSync[12] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };

int Decoder::nextFrame(uint16_t *rgb, int pitchPx) {
	if (!_file) return -1;
	const uint8_t *s;
	while (readSector(&s)) {
		if (memcmp(s, kSync, 12) != 0) continue;
		int channel = s[0x11];
		if (channel >= 32) continue;
		int type = s[0x12] & 0x0e;
		if (type == 0x04) {                          /* audio XA */
			if (_cb) _cb(s + 24, (s[19] & 1) != 0, (s[19] & 4) != 0, _user);
			continue;
		}
		if (type != 0x08 && type != 0x02) continue;   /* données / vidéo */
		if (s[24] != 0x60 || s[25] != 0x01 || s[26] != 0x01 || s[27] != 0x80) continue;
		if (_videoChannel < 0) _videoChannel = channel;
		if (channel != _videoChannel) continue;
		int cur = s[28] | (s[29] << 8), count = s[30] | (s[31] << 8);
		uint32_t fno = s[32] | (s[33] << 8) | (s[34] << 16) | ((uint32_t)s[35] << 24);
		int w = s[40] | (s[41] << 8), h = s[42] | (s[43] << 8);
		if (count <= 0 || count > 4096 || cur >= count) continue;
		if (cur == 0) {                              /* début d'image */
			_frameNo = fno; _frameChunks = count; _frameNext = 0; _frameLen = 0;
			_w = w; _h = h;
		} else if (cur != _frameNext || count != _frameChunks) {
			_frameChunks = 0; _frameNext = 0; continue;   /* image incomplète : on attend la suivante */
		}
		if (_frameChunks == 0) continue;
		uint32_t need = (uint32_t)(cur + 1) * CHUNK;
		if (need + 64 > _frameCap) {
			uint8_t *n = (uint8_t *)realloc(_frame, need + 4 * CHUNK);
			if (!n) return -1;
			_frame = n; _frameCap = need + 4 * CHUNK;
		}
		memcpy(_frame + (uint32_t)cur * CHUNK, s + HDR, CHUNK);
		_frameLen = need; _frameNext = cur + 1;
		if (_frameNext == _frameChunks) {
			_frameChunks = 0; _frameNext = 0;
			if (!rgb) return 1;                       /* image sautée : flux consommé sans décodage */
			if (decodeFrame(rgb, pitchPx)) return 1;
		}
	}
	return 0;
}

bool Decoder::probeSize(int *w, int *h) {
	if (!_file) return false;
	uint32_t save = _pos; uint32_t saveSec = _sectors;
	const uint8_t *s; bool ok = false;
	for (int i = 0; i < 400 && readSector(&s); i++) {
		if (memcmp(s, kSync, 12) != 0) continue;
		int t = s[0x12] & 0x0e;
		if ((t == 0x08 || t == 0x02) && s[24] == 0x60 && s[25] == 0x01 && s[26] == 0x01 && s[27] == 0x80) {
			*w = s[40] | (s[41] << 8); *h = s[42] | (s[43] << 8); ok = true; break;
		}
	}
	_pos = save; _sectors = saveSec;
	return ok && *w > 0 && *h > 0;
}

/* ---- décodage d'une image ---- */
struct BlockCtx { Bits *b; const uint16_t *vlc, *vlc8; int lastDC[3]; int scale; int version; bool bad; };

static inline int signExt10(uint32_t v) { return (int)(v << 22) >> 22; }

/* IDCT 8 points : sorties x et 7-x par symétrie pair/impair (32 multiplications au lieu de 64) */
static inline void idct8(const float *in, float *out) {
	float e0 = in[0] * kM[0][0] + in[2] * kM[0][2] + in[4] * kM[0][4] + in[6] * kM[0][6];
	float e1 = in[0] * kM[1][0] + in[2] * kM[1][2] + in[4] * kM[1][4] + in[6] * kM[1][6];
	float e2 = in[0] * kM[2][0] + in[2] * kM[2][2] + in[4] * kM[2][4] + in[6] * kM[2][6];
	float e3 = in[0] * kM[3][0] + in[2] * kM[3][2] + in[4] * kM[3][4] + in[6] * kM[3][6];
	float o0 = in[1] * kM[0][1] + in[3] * kM[0][3] + in[5] * kM[0][5] + in[7] * kM[0][7];
	float o1 = in[1] * kM[1][1] + in[3] * kM[1][3] + in[5] * kM[1][5] + in[7] * kM[1][7];
	float o2 = in[1] * kM[2][1] + in[3] * kM[2][3] + in[5] * kM[2][5] + in[7] * kM[2][7];
	float o3 = in[1] * kM[3][1] + in[3] * kM[3][3] + in[5] * kM[3][5] + in[7] * kM[3][7];
	out[0] = e0 + o0; out[7] = e0 - o0;
	out[1] = e1 + o1; out[6] = e1 - o1;
	out[2] = e2 + o2; out[5] = e2 - o2;
	out[3] = e3 + o3; out[4] = e3 - o3;
}

static inline uint8_t clampPix(float s) {
	int iv = (int)(s + 128.5f);                 /* arrondi au plus proche */
	return (uint8_t)(iv < 0 ? 0 : iv > 255 ? 255 : iv);
}

static void decodeBlock(BlockCtx &c, uint8_t *out, int pitch, int plane) {
	int coef[64];
	memset(coef, 0, sizeof(coef));
	Bits &b = *c.b;
	/* DC */
	if (c.version == 2) {
		coef[0] = signExt10(b.get(10));
	} else {
		const uint8_t *codes = plane == 2 ? kStrDcLumaCode : kStrDcChromaCode;
		const uint8_t *lens = plane == 2 ? kStrDcLumaLen : kStrDcChromaLen;
		uint32_t w = b.peek16();
		int sym = -1;
		for (int i = 0; i < 9; i++) if ((w >> (16 - lens[i])) == codes[i]) { sym = i; b.skip(lens[i]); break; }
		if (sym < 0) { c.bad = true; return; }
		int dc = 0;
		if (kStrDcBits[sym] != 0) {
			bool neg = b.get(1) == 0;
			dc = (int)b.get(kStrDcBits[sym] - 1);
			dc += neg ? -(int)kStrDcNeg[sym] : (int)kStrDcPos[sym];
		}
		c.lastDC[plane] += dc * 4;
		coef[0] = c.lastDC[plane];
	}
	/* AC : positions (dans le bloc) des coefficients non nuls, pour ne dequantifier que ceux-là */
	float d[64];
	int rowMask = 0, nz = 0;
	{
		static bool inv = false; static uint8_t unzz[64];
		if (!inv) { for (int i = 0; i < 64; i++) unzz[kZigZag[i]] = (uint8_t)i; inv = true; }
		memset(d, 0, sizeof(d));
		int idx = 1;
		const float sc = (float)c.scale * 0.125f;
		while (!b.eos()) {
			uint32_t w = b.peek16();
			uint16_t e = c.vlc8[w >> 8];
			if (!e) e = c.vlc[w];
			if (!e) { c.bad = true; return; }
			int code = (e & 0xff) - 1;
			b.skip(e >> 8);
			uint16_t sym = kStrAcSym[code];
			if (sym == 0xFFFE) break;                          /* fin de bloc */
			int run, level;
			if (sym == 0xFFFF) {                               /* échappement */
				run = (int)b.get(6);
				level = signExt10(b.get(10));
			} else {
				run = sym >> 8; level = sym & 0xff;
				if (b.get(1)) level = -level;
			}
			idx += run;
			if (idx > 63) { c.bad = true; return; }
			int pos = unzz[idx++];
			if (level) { d[pos] = (float)level * kQuant[pos] * sc; rowMask |= 1 << (pos >> 3); nz++; }
		}
	}
	d[0] = (float)(coef[0] * kQuant[0]);
	if (nz == 0) {                                          /* DC seul : bloc uniforme */
		int p = (int)(d[0] * 0.125f + (d[0] < 0 ? -0.5f : 0.5f)) + 128;
		p = p < 0 ? 0 : p > 255 ? 255 : p;
		for (int y = 0; y < 8; y++) { uint32_t q = (uint32_t)p * 0x01010101u; uint8_t *o = out + y * pitch; memcpy(o, &q, 4); memcpy(o + 4, &q, 4); }
		return;
	}
	rowMask |= 1;
	float t[8][8];                                          /* t[v][x] : lignes déjà transformées */
	for (int v = 0; v < 8; v++) {
		if (rowMask & (1 << v)) idct8(d + v * 8, t[v]);
		else for (int x = 0; x < 8; x++) t[v][x] = 0.f;
	}
	if (rowMask == 1) {                                     /* seule la ligne 0 : colonnes constantes */
		for (int x = 0; x < 8; x++) {
			uint8_t p = clampPix(t[0][x] * kM[0][0]);
			for (int y = 0; y < 8; y++) out[y * pitch + x] = p;
		}
		return;
	}
	for (int x = 0; x < 8; x++) {
		float col[8], res[8];
		for (int v = 0; v < 8; v++) col[v] = t[v][x];
		idct8(col, res);
		for (int y = 0; y < 8; y++) out[y * pitch + x] = clampPix(res[y]);
	}
}

/* tables de conversion YCbCr -> RGB555 (plage complète) */
static int s_crR[256], s_crG[256], s_cbG[256], s_cbB[256];
static uint16_t s_tR[768], s_tG[768], s_tB[768];
static bool s_cvtInit = false;
static void initCvt() {
	if (s_cvtInit) return;
	for (int i = 0; i < 256; i++) {
		int v = i - 128;
		s_crR[i] = (91881 * v) >> 16; s_crG[i] = (46802 * v) >> 16;
		s_cbG[i] = (22554 * v) >> 16; s_cbB[i] = (116130 * v) >> 16;
	}
	for (int i = 0; i < 768; i++) {
		int v = i - 256; v = v < 0 ? 0 : v > 255 ? 255 : v; v >>= 3;
		s_tR[i] = (uint16_t)(v << 10); s_tG[i] = (uint16_t)(v << 5); s_tB[i] = (uint16_t)v;
	}
	s_cvtInit = true;
}

bool Decoder::decodeFrame(uint16_t *rgb, int pitchPx) {
	if (_frameLen < 8 || _w <= 0 || _h <= 0) return false;
	initCvt();
	int mbW = (_w + 15) / 16, mbH = (_h + 15) / 16;
	if (mbW != _mbW || mbH != _mbH || !_y) {
		free(_y); free(_cb2); free(_cr); free(_rowCr); free(_rowCb);
		_mbW = mbW; _mbH = mbH;
		_y = (uint8_t *)malloc((size_t)mbW * mbH * 256);
		_cb2 = (uint8_t *)malloc((size_t)mbW * mbH * 64);
		_cr = (uint8_t *)malloc((size_t)mbW * mbH * 64);
		_rowCr = (uint16_t *)malloc((size_t)(mbW * 8 + 2) * 2);
		_rowCb = (uint16_t *)malloc((size_t)(mbW * 8 + 2) * 2);
		if (!_y || !_cb2 || !_cr || !_rowCr || !_rowCb) return false;
	}
	memset(_frame + _frameLen, 0, 16);                                /* gardes de zéros */
	Bits b; b.w = (const u16a *)_frame; b.nwords = _frameLen / 2; b.pos = 32;      /* on saute le mot inconnu et 0x3800 */
	int scale = (int)b.get(16);
	int version = (int)b.get(16);
	if (version != 2 && version != 3) return false;
	BlockCtx c; c.b = &b; c.vlc = _vlc; c.vlc8 = _vlc8; c.lastDC[0] = c.lastDC[1] = c.lastDC[2] = 0; c.scale = scale; c.version = version; c.bad = false;
	int pitchY = mbW * 16, pitchC = mbW * 8;
	for (int mx = 0; mx < mbW && !c.bad; mx++) {
		for (int my = 0; my < mbH && !c.bad; my++) {
			decodeBlock(c, _cr + (my * pitchC + mx) * 8, pitchC, 0);
			decodeBlock(c, _cb2 + (my * pitchC + mx) * 8, pitchC, 1);
			uint8_t *y0 = _y + (my * pitchY + mx) * 16;
			decodeBlock(c, y0, pitchY, 2);
			decodeBlock(c, y0 + 8, pitchY, 2);
			decodeBlock(c, y0 + 8 * pitchY, pitchY, 2);
			decodeBlock(c, y0 + 8 * pitchY + 8, pitchY, 2);
		}
	}
	/* YCbCr 4:2:0 -> RGB555 ; chrominance : voisin le plus proche, ou interpolée (3/4 + 1/4) */
	const int cw = (_w + 1) / 2, lastRow = (_h + 1) / 2 - 1;
	for (int y = 0; y < _h; y++) {
		const uint8_t *yr = _y + y * pitchY;
		const int cy = y >> 1;
		const uint8_t *cbr = _cb2 + cy * pitchC, *crr = _cr + cy * pitchC;
		uint16_t *dst = rgb + (size_t)y * pitchPx;
		if (_smooth) {
			int ny = (y & 1) ? (cy < lastRow ? cy + 1 : cy) : (cy > 0 ? cy - 1 : cy);
			const uint8_t *cbn = _cb2 + ny * pitchC, *crn = _cr + ny * pitchC;
			for (int k = 0; k < cw; k++) { _rowCr[k] = (uint16_t)(3 * crr[k] + crn[k]); _rowCb[k] = (uint16_t)(3 * cbr[k] + cbn[k]); }
		}
		for (int k = 0; k < cw; k++) {
			int x = 2 * k;
			int crL, crR, cbL, cbR;
			if (_smooth) {
				int p = k > 0 ? k - 1 : 0, n = k < cw - 1 ? k + 1 : k;
				crL = (3 * _rowCr[k] + _rowCr[p] + 8) >> 4; crR = (3 * _rowCr[k] + _rowCr[n] + 8) >> 4;
				cbL = (3 * _rowCb[k] + _rowCb[p] + 8) >> 4; cbR = (3 * _rowCb[k] + _rowCb[n] + 8) >> 4;
			} else { crL = crR = crr[k]; cbL = cbR = cbr[k]; }
			int Y = yr[x] + 256;
			dst[x] = s_tR[Y + s_crR[crL]] | s_tG[Y - s_cbG[cbL] - s_crG[crL]] | s_tB[Y + s_cbB[cbL]];
			if (x + 1 < _w) {
				Y = yr[x + 1] + 256;
				dst[x + 1] = s_tR[Y + s_crR[crR]] | s_tG[Y - s_cbG[cbR] - s_crG[crR]] | s_tB[Y + s_cbB[cbR]];
			}
		}
	}
	return true;
}

}
