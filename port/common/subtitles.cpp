#include "subtitles.h"
#include "subfont.h"
#include <stdlib.h>
#include <string.h>

namespace Subs {

Track::~Track() { free(_cues); }

static bool parseTime(const char *&p, uint32_t *out) {
	/* [mm:]ss[.mmm] */
	uint32_t a = 0; int n = 0;
	while (*p >= '0' && *p <= '9') { a = a * 10 + (uint32_t)(*p - '0'); p++; n++; }
	if (!n) return false;
	uint32_t sec = a, ms = 0;
	if (*p == ':') {
		p++; uint32_t b = 0; int m = 0;
		while (*p >= '0' && *p <= '9') { b = b * 10 + (uint32_t)(*p - '0'); p++; m++; }
		if (!m) return false;
		sec = a * 60 + b;
	}
	if (*p == '.' || *p == ',') {
		p++; int digits = 0;
		while (*p >= '0' && *p <= '9') { if (digits < 3) { ms = ms * 10 + (uint32_t)(*p - '0'); digits++; } p++; }
		while (digits++ < 3) ms *= 10;
	}
	*out = sec * 1000 + ms;
	return true;
}

bool Track::parse(const char *d, size_t size) {
	free(_cues); _cues = nullptr; _n = 0; _last = 0;
	int cap = 0;
	size_t i = 0;
	if (size >= 3 && (uint8_t)d[0] == 0xEF && (uint8_t)d[1] == 0xBB && (uint8_t)d[2] == 0xBF) i = 3;   /* BOM UTF-8 */
	while (i < size) {
		size_t e = i; while (e < size && d[e] != '\n' && d[e] != '\r') e++;
		char line[300]; size_t L = e - i; if (L >= sizeof line) L = sizeof line - 1;
		memcpy(line, d + i, L); line[L] = 0;
		i = e; while (i < size && (d[i] == '\n' || d[i] == '\r')) i++;
		const char *p = line; while (*p == ' ' || *p == '\t') p++;
		if (!*p || *p == '#') continue;
		uint32_t a, b;
		if (!parseTime(p, &a)) continue;
		while (*p == ' ' || *p == '\t') p++;
		if (!parseTime(p, &b) || b <= a) continue;
		while (*p == ' ' || *p == '\t') p++;
		if (!*p) continue;
		if (_n == cap) { cap = cap ? cap * 2 : 16; _cues = (Cue *)realloc(_cues, (size_t)cap * sizeof(Cue)); if (!_cues) { _n = 0; return false; } }
		Cue &c = _cues[_n++]; c.startMs = a; c.endMs = b;
		strncpy(c.text, p, sizeof c.text - 1); c.text[sizeof c.text - 1] = 0;
	}
	return _n > 0;
}

const char *Track::textAt(uint32_t ms) {
	if (!_n) return nullptr;
	/* lecture en general croissante : on repart du dernier indice */
	if (_last >= _n || _cues[_last].startMs > ms) _last = 0;
	for (int k = _last; k < _n; k++) {
		if (_cues[k].startMs > ms) break;
		if (ms < _cues[k].endMs) { _last = k; return _cues[k].text; }
	}
	return nullptr;
}

/* UTF-8 -> Latin-1 (les autres caracteres deviennent '?') */
static int g_pending = 0;   /* second caractere d'une ligature (œ -> o e) */
static int nextChar(const char *&s) {
	if (g_pending) { int p = g_pending; g_pending = 0; return p; }
	unsigned char c = (unsigned char)*s++;
	if (c < 0x80) return c;
	if ((c & 0xE0) == 0xC0 && ((unsigned char)*s & 0xC0) == 0x80) {
		int cp = ((c & 0x1F) << 6) | ((unsigned char)*s++ & 0x3F);
		if (cp == 0x2019 || cp == 0x2018) return '\'';
		if (cp == 0x153) { g_pending = 'e'; return 'o'; }
		if (cp == 0x152) { g_pending = 'E'; return 'O'; }
		return cp < 256 ? cp : '?';
	}
	if ((c & 0xF0) == 0xE0 && ((unsigned char)*s & 0xC0) == 0x80) {
		int cp = ((c & 0x0F) << 12) | (((unsigned char)s[0] & 0x3F) << 6) | ((unsigned char)s[1] & 0x3F); s += 2;
		if (cp == 0x2019 || cp == 0x2018) return '\'';
		if (cp == 0x2026) return '.';
		if (cp == 0x2013 || cp == 0x2014) return '-';
		return '?';
	}
	while (((unsigned char)*s & 0xC0) == 0x80) s++;
	return '?';
}

static int lineWidth(const int *g, int n) {
	int w = 0; for (int i = 0; i < n; i++) w += SubFont::ADV[g[i] - SubFont::FIRST];
	return w;
}

/* decoupe en lignes au caractere '|' ; renvoie le nombre de lignes (max 3) */
static int split(const char *utf8, int lines[3][96], int len[3]) {
	int nl = 0; len[0] = 0;
	const char *s = utf8;
	while (*s) {
		int c = nextChar(s);
		if (c == '|') { if (nl < 2) { nl++; len[nl] = 0; } continue; }
		if (c < SubFont::FIRST || c >= SubFont::FIRST + SubFont::COUNT) c = '?';
		if (len[nl] < 95) lines[nl][len[nl]++] = c;
	}
	return nl + 1;
}

int textHeight(const char *utf8) {
	int lines[3][96], len[3]; int n = split(utf8, lines, len);
	return n * (SubFont::H + 1);
}

static void putGlyph(uint16_t *px, int pitch, int w, int h, int x, int y, int c, uint16_t col) {
	const uint32_t *rows = SubFont::ROWS[c - SubFont::FIRST];
	for (int r = 0; r < SubFont::H; r++) {
		int yy = y + r; if (yy < 0 || yy >= h) continue;
		uint32_t bits = rows[r];
		while (bits) {
			int b = __builtin_ctz(bits); bits &= bits - 1;
			int xx = x + b; if (xx >= 0 && xx < w) px[(size_t)yy * pitch + xx] = col;
		}
	}
}

int drawText(uint16_t *px, int pitch, int w, int h, const char *utf8, uint16_t fg, uint16_t outline) {
	int lines[3][96], len[3]; int n = split(utf8, lines, len);
	const int lh = SubFont::H + 1;
	int y0 = h - n * lh - 2;
	for (int l = 0; l < n; l++) {
		int tw = lineWidth(lines[l], len[l]);
		int x = (w - tw) / 2; if (x < 1) x = 1;
		int y = y0 + l * lh;
		/* contour : le glyphe en noir decale dans les 8 directions, puis en couleur au centre */
		for (int pass = 0; pass < 2; pass++) {
			int cx = x;
			for (int i = 0; i < len[l]; i++) {
				int c = lines[l][i];
				if (pass == 0) {
					for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) if (dx || dy) putGlyph(px, pitch, w, h, cx + dx, y + dy, c, outline);
				} else putGlyph(px, pitch, w, h, cx, y, c, fg);
				cx += SubFont::ADV[c - SubFont::FIRST];
			}
		}
	}
	return n;
}

}
