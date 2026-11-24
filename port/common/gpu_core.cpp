#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "bdprof.h"
#include "gpu_core.h"
#include <stdlib.h>
#include <algorithm>
#include <cmath>
int g_hdSprWhy = 0;
int g_hdSprExpectW = 0, g_hdSprExpectH = 0;   /* taille d'origine de la trame demandée (0 = pas de contrôle) */   /* raison du dernier refus de hdSprite (journal) */
#if defined(__DREAMCAST__) || defined(_arch_dreamcast)
#include <malloc.h>
#define BD_ALIGNED_ALLOC(n) memalign(32, (n))
#else
#define BD_ALIGNED_ALLOC(n) malloc(n)
#endif

namespace Gpu {

using Dragons::AlphaBlendMode;
using Dragons::NONE;
using Dragons::NORMAL;

enum { STAGE_PX = 512 * 256, TILE = 256, MAX_OPS = 4096 };

static inline int pow2(int n) { int p = 8; while (p < n) p <<= 1; return p; }

static void hashBytes(const byte *p, size_t n, uint32 &h1, uint32 &h2) {
	size_t i = 0;
	if (((uintptr_t)p & 3) == 0) {
		for (; i + 4 <= n; i += 4) {
			uint32 w = *(const uint32 *)(p + i);
			h1 = (h1 ^ w) * 16777619u;
			h2 = (h2 + w) * 2654435761u;
			h2 ^= h2 >> 15;
		}
	}
	for (; i < n; i++) {
		h1 = (h1 ^ p[i]) * 16777619u;
		h2 = (h2 + p[i]) * 2654435761u;
		h2 ^= h2 >> 15;
	}
}

static uint32 hashPalette(const byte *pal) {
	uint32 a = 2166136261u, b = 0x9e3779b9u;
	hashBytes(pal, 512, a, b);
	return a ^ (b * 3u);
}

/* couleur engine (RGB555, bit 15 = demi-transparent) → ARGB4444 */
static inline uint16 pack4444(uint16 c, int a) {
	return (uint16)((a << 12) | (((c >> 11) & 15) << 8) | (((c >> 6) & 15) << 4) | ((c >> 1) & 15));
}

Core::Core(Sink *sink, size_t budgetBytes)
	: _sink(sink), _budget(budgetBytes), _used(0), _frame(0), _videoSeq(0) {
	_stage = (uint16 *)BD_ALIGNED_ALLOC(STAGE_PX * 2);
	_ops.reserve(256);
}

Core::~Core() {
	for (auto &e : _tex) _sink->texDestroy(e.second.handle);
	for (auto &l : _layers) for (auto &t : l.second.tiles) _sink->texDestroy(t.second.handle);
	for (auto &l : _hdLayers) for (auto &t : l.second.l.tiles) _sink->texDestroy(t.second.handle);
	for (auto &t : _hdPages) _sink->texDestroy(t.second.handle);
	for (auto &p : _pending) _sink->texDestroy(p.handle);
	free(_stage);
}

/* ---- mémoire ---- */

void Core::dropOpsUsing(uint32 handle) {
	_ops.erase(std::remove_if(_ops.begin(), _ops.end(), [handle](const Op &o) { return !o.solid && o.tex == handle; }), _ops.end());
}

void Core::releaseTex(Tex &t) {
	dropOpsUsing(t.handle);
	_pending.push_back({ t.handle, t.bytes, _frame });
}

/* Libère la texture la plus ancienne qui n'a pas servi depuis 3 images (le GPU ne la lit plus). */
static bool oldestEvictable(uint32 frame, uint32 lastUse) { return lastUse + 3 <= frame; }

bool Core::makeRoom(size_t need) {
	while (_used + need > _budget) {
		Tex *best = nullptr; std::map<Key, Tex>::iterator bi = _tex.end();
		LayerEntry *bl = nullptr; int bidx = 0; bool hp = false;
		for (auto it = _tex.begin(); it != _tex.end(); ++it)
			if (oldestEvictable(_frame, it->second.lastUse) && (!best || it->second.lastUse < best->lastUse)) { best = &it->second; bi = it; hp = false; bl = nullptr; }
		for (auto &l : _layers)
			for (auto &t : l.second.tiles)
				if (oldestEvictable(_frame, t.second.lastUse) && (!best || t.second.lastUse < best->lastUse)) { best = &t.second; bl = &l.second; bidx = t.first; bi = _tex.end(); hp = false; }
		for (auto &l : _hdLayers)
			for (auto &t : l.second.l.tiles)
				if (oldestEvictable(_frame, t.second.lastUse) && (!best || t.second.lastUse < best->lastUse)) { best = &t.second; bl = &l.second.l; bidx = t.first; bi = _tex.end(); hp = false; }
		for (auto &t : _hdPages)
			if (oldestEvictable(_frame, t.second.lastUse) && (!best || t.second.lastUse < best->lastUse)) { best = &t.second; bl = nullptr; bidx = (int)t.first; bi = _tex.end(); hp = true; }
		if (!best) return false;
		dropOpsUsing(best->handle);
		_sink->texDestroy(best->handle);
		_used -= best->bytes;
		stats.texEvicted++;
		if (hp) _hdPages.erase((uint32)bidx); else if (bl) bl->tiles.erase(bidx); else _tex.erase(bi);
	}
	return true;
}

uint32 Core::createTex(int w, int h, int fmt, size_t &bytes) {
	bytes = (size_t)w * h * 2;
	if (!makeRoom(bytes)) { stats.texFailed++; return 0; }
	uint32 hd;
	{ BdProf::Scope prof(BdProf::TEX); hd = _sink->texCreate(w, h, fmt, _stage); }
	while (!hd) {                       /* mémoire vidéo réellement pleine : libérer et réessayer */
		size_t keep = _budget;
		_budget = _used > bytes ? _used - 1 : 0;   /* force une éviction */
		bool ok = makeRoom(bytes);
		_budget = keep;
		if (!ok) break;
		hd = _sink->texCreate(w, h, fmt, _stage);
	}
	if (!hd) { stats.texFailed++; return 0; }
	_used += bytes;
	stats.texCreated++;
	stats.bytesCreated += bytes;
	return hd;
}

/* Plus grand rectangle de cases vraies d'une grille w×h (méthode des histogrammes). Renvoie l'aire (0 : aucun). */
template <class F>
static int largestRect(int w, int h, F ok, int &rx0, int &ry0, int &rx1, int &ry1) {
	std::vector<int> ht(w + 1, 0), st; st.reserve(w + 1);
	int best = 0;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) ht[x] = ok(x, y) ? ht[x] + 1 : 0;
		ht[w] = 0; st.clear();
		for (int x = 0; x <= w; x++) {
			while (!st.empty() && ht[st.back()] >= ht[x]) {
				const int hh = ht[st.back()]; st.pop_back();
				const int left = st.empty() ? 0 : st.back() + 1;
				const int area = hh * (x - left);
				if (area > best) { best = area; rx0 = left; rx1 = x; ry1 = y + 1; ry0 = y + 1 - hh; }
			}
			st.push_back(x);
		}
	}
	return best;
}

/* Parties cachées : en partant de la fin, ce qui est sous une partie entièrement opaque dessinée après n'est pas envoyé
   au PVR (sac ouvert : la scène dessous coûtait presque un écran de plus). Une primitive partiellement cachée est découpée
   en au plus 4 bandes autour de la partie cachée. */
void Core::occludeOps() {
	struct R { float x0, y0, x1, y1; };
	R occ[16]; int n = 0;
	_occOut.clear();
	auto sub = [](const Op &o, float x0, float y0, float x1, float y1) {
		Op r = o;
		const float du = (o.u1 - o.u0) / (o.x1 - o.x0), dv = (o.v1 - o.v0) / (o.y1 - o.y0);
		r.x0 = x0; r.x1 = x1; r.y0 = y0; r.y1 = y1;
		r.u0 = o.u0 + du * (x0 - o.x0); r.u1 = o.u0 + du * (x1 - o.x0);
		r.v0 = o.v0 + dv * (y0 - o.y0); r.v1 = o.v0 + dv * (y1 - o.y0);
		r.ox0 = r.ox1 = 0;
		return r;
	};
	for (int i = (int)_ops.size() - 1; i >= 0; i--) {
		const Op &o0 = _ops[i];
		Op work[48]; int nw = 0; work[nw++] = o0;
		for (int k = 0; k < n; k++) {
			const R &r = occ[k];
			int m = nw;
			for (int w = 0; w < m; w++) {
				Op &o = work[w];
				if (o.x0 >= o.x1) continue;
				const float ix0 = std::max(o.x0, r.x0), ix1 = std::min(o.x1, r.x1), iy0 = std::max(o.y0, r.y0), iy1 = std::min(o.y1, r.y1);
				if (ix0 >= ix1 || iy0 >= iy1) continue;
				if ((ix1 - ix0) * (iy1 - iy0) < 1024 && !(ix0 == o.x0 && ix1 == o.x1 && iy0 == o.y0 && iy1 == o.y1)) continue;   /* petit recouvrement : pas la peine */
				if (nw + 4 > 48) continue;
				const Op c = o;
				o.x1 = o.x0;   /* remplacée par les bandes visibles */
				if (c.y0 < iy0) work[nw++] = sub(c, c.x0, c.y0, c.x1, iy0);
				if (iy1 < c.y1) work[nw++] = sub(c, c.x0, iy1, c.x1, c.y1);
				if (c.x0 < ix0) work[nw++] = sub(c, c.x0, iy0, ix0, iy1);
				if (ix1 < c.x1) work[nw++] = sub(c, ix1, iy0, c.x1, iy1);
			}
		}
		for (int w = nw - 1; w >= 0; w--) if (work[w].x0 < work[w].x1) _occOut.push_back(work[w]);
		if (o0.ox0 < o0.ox1 && o0.oy0 < o0.oy1 && n < 16 && (o0.ox1 - o0.ox0) * (o0.oy1 - o0.oy0) >= 1024)
			occ[n++] = { o0.ox0, o0.oy0, o0.ox1, o0.oy1 };
	}
	_ops.assign(_occOut.rbegin(), _occOut.rend());
}

/* Une primitive entièrement opaque peut passer dans la liste opaque du PVR (rendue avant la liste translucide, bien moins
   coûteuse) si rien de translucide dessiné avant elle ne la chevauche : l'ordre visible ne change pas. (Décor HD du
   milieu qui cache tout le fond : le fond est retiré par occludeOps, le milieu devient opaque.) */
void Core::promoteOpaque() {
	struct R { float x0, y0, x1, y1; };
	static R tr[MAX_OPS]; int n = 0;
	for (Op &o : _ops) {
		if (o.x0 >= o.x1 || o.y0 >= o.y1) continue;
		if (!o.opq && o.full && o.blend == BL_ALPHA && !o.solid) {
			bool hit = false;
			for (int k = 0; k < n && !hit; k++) hit = tr[k].x0 < o.x1 && tr[k].x1 > o.x0 && tr[k].y0 < o.y1 && tr[k].y1 > o.y0;
			if (!hit) { o.opq = true; continue; }
		}
		if (!o.opq && n < MAX_OPS) tr[n++] = { o.x0, o.y0, o.x1, o.y1 };
	}
}

/* ---- conversion ---- */

/* Remplit _stage (tw×th) depuis get(x, y) = couleur engine 16 bits (0 = transparent).
 * mode 0 : tout opaque ; mode 1 : bit 15 = demi-transparent (→ ARGB4444 si présent) ; mode 2 : additif, tout opaque. */
template <class F>
bool Core::build(int w, int h, int mode, F get, int &tw, int &th, int &fmt) {
	tw = pow2(w); th = pow2(h);
	if (tw * th > STAGE_PX) return false;
	memset(_stage, 0, (size_t)tw * th * 2);
	bool semi = false;
	for (int y = 0; y < h; y++) {
		uint16 *d = _stage + y * tw;
		for (int x = 0; x < w; x++) {
			uint16 c = get(x, y);
			if (!c) continue;
			if (mode == 1 && (c & 0x8000)) semi = true;
			d[x] = (c & 0x7fff) | 0x8000;
		}
	}
	fmt = FMT_1555;
	if (semi) {
		fmt = FMT_4444;
		for (int y = 0; y < h; y++) {
			uint16 *d = _stage + y * tw;
			for (int x = 0; x < w; x++) {
				uint16 c = get(x, y);
				d[x] = c ? pack4444(c, (c & 0x8000) ? 8 : 15) : 0;
			}
		}
	}
	return true;
}

/* ---- primitives ---- */

void Core::clear() {
	_ops.clear();
}

void Core::setLayerClip(bool on, int x0, int y0, int x1, int y1) {
	_clipOn = on; _cx0 = (float)x0; _cy0 = (float)y0; _cx1 = (float)x1; _cy1 = (float)y1;
}

void Core::addQuad(uint32 tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, int blend) {
	const bool occNext = _occNext; _occNext = false;   /* consommé même si le quad est rogné à rien */
	if (_clipOn) {   /* rectangle d'écran actif (couches partiellement en HD) : on rogne le quad et ses coordonnées de texture */
		const float w = x1 - x0, h = y1 - y0;
		if (w <= 0 || h <= 0) return;
		float nx0 = x0 < _cx0 ? _cx0 : x0, nx1 = x1 > _cx1 ? _cx1 : x1, ny0 = y0 < _cy0 ? _cy0 : y0, ny1 = y1 > _cy1 ? _cy1 : y1;
		if (nx0 >= nx1 || ny0 >= ny1) return;
		const float nu0 = u0 + (u1 - u0) * (nx0 - x0) / w, nu1 = u0 + (u1 - u0) * (nx1 - x0) / w;
		const float nv0 = v0 + (v1 - v0) * (ny0 - y0) / h, nv1 = v0 + (v1 - v0) * (ny1 - y0) / h;
		x0 = nx0; x1 = nx1; y0 = ny0; y1 = ny1; u0 = nu0; u1 = nu1; v0 = nv0; v1 = nv1;
	}
	if (_ops.size() >= MAX_OPS) { _ops.clear(); stats.skipped++; }
	Op o; o.tex = tex; o.x0 = x0; o.y0 = y0; o.x1 = x1; o.y1 = y1; o.u0 = u0; o.v0 = v0; o.u1 = u1; o.v1 = v1;
	o.blend = blend; o.alpha = 255; o.col = 0; o.solid = false; o.opq = _opqNext && blend == BL_ALPHA; o.bank = _bankNext; o.full = _fullNext && blend == BL_ALPHA;
	if (occNext && blend == BL_ALPHA && u1 != u0 && v1 != v0) {
		auto mx = [&](float u) { return x0 + (u - u0) / (u1 - u0) * (x1 - x0); };
		auto my = [&](float v) { return y0 + (v - v0) / (v1 - v0) * (y1 - y0); };
		float a = mx(_occ[0]), b = mx(_occ[2]), c = my(_occ[1]), d = my(_occ[3]);
		o.ox0 = std::max(std::min(a, b), x0) + 1; o.ox1 = std::min(std::max(a, b), x1) - 1;   /* 1 pixel de marge (filtrage) */
		o.oy0 = std::max(std::min(c, d), y0) + 1; o.oy1 = std::min(std::max(c, d), y1) - 1;
	}
	_ops.push_back(o);
}

static inline int modeOf(AlphaBlendMode a) { return a == NONE ? 0 : (a == NORMAL ? 1 : 2); }

void Core::sprite(const Graphics::Surface &s, const byte *palette, int x, int y, const Common::Rect &r,
                  bool flipX, AlphaBlendMode alpha, uint16 scale) {
	BdProf::Sub profSub(BdProf::S_SPRITE);
	BdProf::Scope profPrep(BdProf::PREP);
	if (s.w <= 0 || s.h <= 0 || s.format.bytesPerPixel != 1) return;
	int l = std::max<int>(0, r.left), t = std::max<int>(0, r.top);
	int rr = std::min<int>(s.w, r.right), bb = std::min<int>(s.h, r.bottom);
	if (rr <= l || bb <= t) return;
	int dw = rr - l, dh = bb - t;
	if (scale != DRAGONS_ENGINE_SPRITE_100_PERCENT_SCALE) {
		dw = dw * scale / DRAGONS_ENGINE_SPRITE_100_PERCENT_SCALE;
		dh = dh * scale / DRAGONS_ENGINE_SPRITE_100_PERCENT_SCALE;
	}
	if (dw <= 0 || dh <= 0) return;

	const int mode = modeOf(alpha);
	const byte *px = (const byte *)s.getPixels();
	uint32 h1 = 2166136261u, h2 = 0x9e3779b9u;
	for (int j = 0; j < s.h; j++) hashBytes(px + j * s.pitch, s.w, h1, h2);
	Key key = {{ h1, h2, (uint32)((s.w << 16) | s.h), hashPalette(palette), (uint32)mode, 0 }};

	auto it = _tex.find(key);
	if (it == _tex.end()) {
		uint16 pal[256];
		memcpy(pal, palette, 512);
		int tw, th, fmt;
		auto get = [&](int xx, int yy) -> uint16 { return pal[px[yy * s.pitch + xx]]; };
		/* sprite surtout demi-transparent (brume du village…) : traînées de pixels durs -> flou doux (couleur pondérée
		   par l'opacité, et opacité floutées) */
		int nS = 0, nV = 0;
		if (mode == 1) for (int yy = 0; yy < s.h; yy++) for (int xx = 0; xx < s.w; xx++) { const uint16 c = get(xx, yy); if (c) { nV++; if (c & 0x8000) nS++; } }
		bool soft = mode == 1 && !Dragons::g_spriteNoSoft && nV > 64 && nS * 4 > nV * 3;   /* jamais pour le texte et ses cadres (FontManager::draw) */
		if (soft) {
			enum { SX = 3, SY = 1 };
			tw = pow2(s.w); th = pow2(s.h);
			if (tw * th > STAGE_PX) soft = false;
			else {
				memset(_stage, 0, (size_t)tw * th * 2);
				for (int yy = 0; yy < s.h; yy++)
					for (int xx = 0; xx < s.w; xx++) {
						int A = 0, R = 0, G = 0, B = 0, n = 0;
						for (int j = -SY; j <= SY; j++) {
							const int y2 = yy + j; if (y2 < 0 || y2 >= s.h) { n += 2 * SX + 1; continue; }
							for (int i = -SX; i <= SX; i++) {
								const int x2 = xx + i; n++;
								if (x2 < 0 || x2 >= s.w) continue;
								const uint16 c = get(x2, y2); if (!c) continue;
								const int a = (c & 0x8000) ? 8 : 15;
								A += a; R += a * (c & 31); G += a * ((c >> 5) & 31); B += a * ((c >> 10) & 31);
							}
						}
						if (!A) continue;
						const uint16 c = (uint16)((R / A) | ((G / A) << 5) | ((B / A) << 10));
						const int a = (A + n / 2) / n;
						if (a > 0) _stage[yy * tw + xx] = pack4444(c, a);
					}
				fmt = FMT_4444;
			}
		}
		if (!soft && !build(s.w, s.h, mode, get, tw, th, fmt)) { stats.skipped++; return; }
		size_t bytes;
		uint32 hd = createTex(tw, th, fmt, bytes);
		if (!hd) { stats.skipped++; return; }
		Tex tx; tx.handle = hd; tx.bytes = bytes; tx.lastUse = _frame;
		{	/* rectangle des pixels visibles, en pixels (le texte est dessiné dans une surface plein écran presque vide) */
			int bx0 = s.w, by0 = s.h, bx1 = 0, by1 = 0;
			for (int yy = 0; yy < s.h; yy++)
				for (int xx = 0; xx < s.w; xx++)
					if (get(xx, yy)) { if (xx < bx0) bx0 = xx; if (xx >= bx1) bx1 = xx + 1; if (yy < by0) by0 = yy; if (yy >= by1) by1 = yy + 1; }
			if (soft && bx1 > bx0) { bx0 = std::max(0, bx0 - 3); bx1 = std::min((int)s.w, bx1 + 3); by0 = std::max(0, by0 - 1); by1 = std::min((int)s.h, by1 + 1); }
			if (bx1 <= bx0) tx.bx0 = tx.bx1 = 0;
			else { tx.bx0 = (float)std::max(0, bx0 - 1); tx.by0 = (float)std::max(0, by0 - 1); tx.bx1 = (float)std::min((int)s.w, bx1 + 1); tx.by1 = (float)std::min((int)s.h, by1 + 1); }   /* marge d'un texel (filtrage) */
		}
		it = _tex.insert(std::make_pair(key, tx)).first;
	}
	it->second.lastUse = _frame;
	{
		const Tex &tt = it->second;
		if (tt.bx0 >= tt.bx1) return;   /* rien de visible */
		if (!flipX && scale == DRAGONS_ENGINE_SPRITE_100_PERCENT_SCALE) {
			const int nl = std::max(l, (int)tt.bx0), nr = std::min(rr, (int)tt.bx1);
			const int nt = std::max(t, (int)tt.by0), nb = std::min(bb, (int)tt.by1);
			if (nl >= nr || nt >= nb) return;
			x += nl - l; y += nt - t; l = nl; rr = nr; t = nt; bb = nb; dw = rr - l; dh = bb - t;
		}
	}

	const float tw = (float)pow2(s.w), th = (float)pow2(s.h);
	float u0 = l / tw, u1 = rr / tw, v0 = t / th, v1 = bb / th;
	if (scale != DRAGONS_ENGINE_SPRITE_100_PERCENT_SCALE && !Dragons::g_spriteExactUV) {
		/* Reproduit l'échantillonnage de Screen::drawScaledSprite : source = arrondi(colonne × (largeur-1) / largeur_dest),
		 * avec le décalage d'une colonne qu'a l'original quand le sprite est retourné. */
		const int rw = rr - l, rh = bb - t;
		const float kx = (rw - 1) / (float)dw, ky = (rh - 1) / (float)dh;
		v0 = (t + 0.5f - 0.5f * ky) / th;
		v1 = (t + (rh - 1) + 0.5f - 0.5f * ky) / th;
		if (flipX) {
			u0 = (l + (dw + 0.5f) * kx + 0.5f) / tw;
			u1 = (l + 0.5f * kx + 0.5f) / tw;
		} else {
			u0 = (l + 0.5f - 0.5f * kx) / tw;
			u1 = (l + (rw - 1) + 0.5f - 0.5f * kx) / tw;
		}
	} else if (flipX) std::swap(u0, u1);
	addQuad(it->second.handle, (float)x, (float)y, (float)(x + dw), (float)(y + dh), u0, v0, u1, v1, mode == 2 ? BL_ADD : BL_ALPHA);
}

void Core::bitmap16(const Graphics::Surface &s, int x, int y, const Common::Rect &r, bool flipX, AlphaBlendMode alpha) {
	BdProf::Sub profSub(BdProf::S_BMP16);
	BdProf::Scope profPrep(BdProf::PREP);
	if (s.w <= 0 || s.h <= 0 || s.format.bytesPerPixel != 2) return;
	int l = std::max<int>(0, r.left), t = std::max<int>(0, r.top);
	int rr = std::min<int>(s.w, r.right), bb = std::min<int>(s.h, r.bottom);
	if (rr <= l || bb <= t) return;

	const int mode = alpha == NONE ? 0 : 1;
	const byte *px = (const byte *)s.getPixels();
	uint32 h1 = 2166136261u, h2 = 0x9e3779b9u;
	for (int j = 0; j < s.h; j++) hashBytes(px + j * s.pitch, s.w * 2, h1, h2);
	Key key = {{ h1, h2, (uint32)((s.w << 16) | s.h), 0, (uint32)(mode + 4), 0 }};

	auto it = _tex.find(key);
	if (it == _tex.end()) {
		int tw, th, fmt;
		auto get = [&](int xx, int yy) -> uint16 { uint16 c; memcpy(&c, px + yy * s.pitch + xx * 2, 2); return c; };
		if (!build(s.w, s.h, mode, get, tw, th, fmt)) { stats.skipped++; return; }
		size_t bytes;
		uint32 hd = createTex(tw, th, fmt, bytes);
		if (!hd) { stats.skipped++; return; }
		Tex tx; tx.handle = hd; tx.bytes = bytes; tx.lastUse = _frame;
		{	/* rectangle des pixels visibles (l'incrustation plein écran du texte est presque toujours vide) */
			int bx0 = s.w, by0 = s.h, bx1 = 0, by1 = 0;
			for (int yy = 0; yy < s.h; yy++) {
				const uint16 *row = (const uint16 *)(px + yy * s.pitch);
				for (int xx = 0; xx < s.w; xx++)
					if (row[xx]) { if (xx < bx0) bx0 = xx; if (xx >= bx1) bx1 = xx + 1; if (yy < by0) by0 = yy; if (yy >= by1) by1 = yy + 1; }
			}
			if (bx1 <= bx0) tx.bx0 = tx.bx1 = 0;
			else { tx.bx0 = (float)std::max(0, bx0 - 1); tx.by0 = (float)std::max(0, by0 - 1); tx.bx1 = (float)std::min((int)s.w, bx1 + 1); tx.by1 = (float)std::min((int)s.h, by1 + 1); }   /* marge d'un texel (filtrage) */   /* en pixels ici */
			int a0, b0, a1, b1;   /* plus grand rectangle opaque (ni transparent ni demi-transparent), un texel de marge */
			if (largestRect(s.w, s.h, [&](int xx, int yy) { const uint16 c = get(xx, yy); return c != 0 && !(mode == 1 && (c & 0x8000)); }, a0, b0, a1, b1) > 64) {
				const float tw2 = (float)pow2(s.w), th2 = (float)pow2(s.h);
				tx.iu0 = (a0 + 1) / tw2; tx.iv0 = (b0 + 1) / th2; tx.iu1 = (a1 - 1) / tw2; tx.iv1 = (b1 - 1) / th2;
			}
		}
		it = _tex.insert(std::make_pair(key, tx)).first;
	}
	it->second.lastUse = _frame;
	{
		const Tex &tt = it->second;
		if (tt.bx0 >= tt.bx1) return;   /* rien de visible */
		if (!flipX) {
			const int nl = std::max(l, (int)tt.bx0), nr = std::min(rr, (int)tt.bx1);
			const int nt = std::max(t, (int)tt.by0), nb = std::min(bb, (int)tt.by1);
			if (nl >= nr || nt >= nb) return;
			x += nl - l; y += nt - t; l = nl; rr = nr; t = nt; bb = nb;
		}
	}

	const float tw = (float)pow2(s.w), th = (float)pow2(s.h);
	float u0 = l / tw, u1 = rr / tw, v0 = t / th, v1 = bb / th;
	if (flipX) std::swap(u0, u1);
	{ const Tex &tt = it->second; if (tt.iu0 < tt.iu1) { _occNext = true; _occ[0] = tt.iu0; _occ[1] = tt.iv0; _occ[2] = tt.iu1; _occ[3] = tt.iv1; } }
	addQuad(it->second.handle, (float)x, (float)y, (float)(x + rr - l), (float)(y + bb - t), u0, v0, u1, v1, BL_ALPHA);
}

void Core::video16(const uint16 *px, int pitchPx, int w, int h, int x, int y, const Common::Rect &r) {
	BdProf::Sub profSub(BdProf::S_VIDEO);
	BdProf::Scope profPrep(BdProf::PREP);
	if (w <= 0 || h <= 0) return;
	int l = std::max<int>(0, r.left), t = std::max<int>(0, r.top);
	int rr = std::min<int>(w, r.right), bb = std::min<int>(h, r.bottom);
	if (rr <= l || bb <= t) return;
	int tw = pow2(w), th = pow2(h);
	if (tw * th > STAGE_PX) { stats.skipped++; return; }
	for (int j = 0; j < h; j++) {                         /* bit 15 = opaque */
		const uint16 *s = px + (size_t)j * pitchPx; uint16 *d = _stage + j * tw;
		for (int i = 0; i < w; i++) d[i] = s[i] | 0x8000;
	}
	size_t bytes;
	uint32 hd = createTex(tw, th, FMT_1555, bytes);
	if (!hd) { stats.skipped++; return; }
	Key key = {{ ++_videoSeq, 0x71de0u, (uint32)((w << 16) | h), 0, 9, 0 }};
	Tex tx = { hd, bytes, _frame };
	_tex.insert(std::make_pair(key, tx));
	addQuad(hd, (float)x, (float)y, (float)(x + rr - l), (float)(y + bb - t), l / (float)tw, t / (float)th, rr / (float)tw, bb / (float)th, BL_ALPHA);
}

void Core::fillRect(uint16 colour, const Common::Rect &r) {
	BdProf::Sub profSub(BdProf::S_FILL);
	if (r.width() <= 0 || r.height() <= 0) return;
	if (_ops.size() >= MAX_OPS) { _ops.clear(); stats.skipped++; }
	Op o; o.tex = 0; o.x0 = r.left; o.y0 = r.top; o.x1 = r.right; o.y1 = r.bottom;
	o.u0 = o.v0 = o.u1 = o.v1 = 0; o.blend = BL_ALPHA; o.alpha = 255; o.col = colour & 0x7fff; o.solid = true;
	_ops.push_back(o);
}

/* ---- décors en tuiles de 256×256 ---- */

Core::Tex *Core::layerTile(LayerEntry &e, int tx, int ty, const Graphics::Surface &s, const uint16 *pal, int mode, int &) {
	int idx = ty * 64 + tx;
	auto it = e.tiles.find(idx);
	if (it != e.tiles.end()) { it->second.lastUse = _frame; return &it->second; }
	const byte *px = (const byte *)s.getPixels();
	const int x0 = tx * TILE, y0 = ty * TILE;
	const int vw = std::min<int>(TILE, s.w - x0), vh = std::min<int>(TILE, s.h - y0);
	int tw, th, fmt;
	/* couche additive (mode 2) : sur PS1 seuls les pixels à bit 15 (STP) s'additionnent, les autres sont pleins. Deux
	   passes : mode 4 = pixels pleins (mélange normal), mode 2 = pixels additifs */
	auto get = [&](int x, int y) -> uint16 {
		if (x >= vw || y >= vh) return 0;
		const uint16 c = pal[px[(y0 + y) * s.pitch + x0 + x]];
		if (mode == 2) return (c & 0x8000) ? c : 0;
		if (mode == 4) return (c & 0x8000) ? 0 : c;
		return c;
	};
	std::vector<uint16> soft;
	if (mode == 2 && e.soft) {
		/* couche additive (brume, nuages) : dessinée en traînées de pixels durs ; un flou (horizontal ±SX en bouclant,
		   vertical ±SY) en fait une brume douce, le filtrage de la carte graphique fait le reste */
		enum { SX = 5, SY = 2 };
		const int W = s.w, H = s.h, rh = vh + 2 * SY;
		std::vector<uint16> hs((size_t)rh * vw * 3);
		for (int r = 0; r < rh; r++) {
			const int sy = std::min(H - 1, std::max(0, y0 + r - SY));
			const byte *row = px + (size_t)sy * s.pitch;
			int a = 0, b = 0, c = 0;
			auto acc = [&](int sx, int k) { const uint16 v0 = pal[row[((sx % W) + W) % W]]; const uint16 v = (v0 & 0x8000) ? (v0 & 0x7fff) : 0; a += k * (v & 31); b += k * ((v >> 5) & 31); c += k * (v >> 10); };
			for (int k = -SX; k <= SX; k++) acc(x0 + k, 1);
			for (int x = 0; x < vw; x++) {
				uint16 *d = &hs[((size_t)r * vw + x) * 3]; d[0] = a; d[1] = b; d[2] = c;
				acc(x0 + x - SX, -1); acc(x0 + x + SX + 1, 1);
			}
		}
		soft.assign((size_t)vw * vh, 0);
		const int n = (2 * SX + 1) * (2 * SY + 1);
		for (int x = 0; x < vw; x++) {
			int a = 0, b = 0, c = 0;
			for (int r = 0; r < 2 * SY + 1; r++) { const uint16 *d = &hs[((size_t)r * vw + x) * 3]; a += d[0]; b += d[1]; c += d[2]; }
			for (int y = 0; y < vh; y++) {
				const int R = (a + n / 2) / n, G = (b + n / 2) / n, B = (c + n / 2) / n;
				const uint16 v = (uint16)(R | (G << 5) | (B << 10));
				soft[(size_t)y * vw + x] = v ? (v | 0x8000) : 0;
				if (y + 1 < vh) {
					const uint16 *o = &hs[((size_t)y * vw + x) * 3], *i2 = &hs[((size_t)(y + 2 * SY + 1) * vw + x) * 3];
					a += i2[0] - o[0]; b += i2[1] - o[1]; c += i2[2] - o[2];
				}
			}
		}
	}
	auto getSoft = [&](int x, int y) -> uint16 { return (x >= vw || y >= vh) ? 0 : soft[(size_t)y * vw + x]; };
	const bool useSoft = mode == 2 && e.soft;
	if (!(useSoft ? build(TILE, TILE, mode, getSoft, tw, th, fmt) : build(TILE, TILE, mode == 4 ? 0 : mode, get, tw, th, fmt))) return nullptr;
	size_t bytes;
	uint32 hd = createTex(tw, th, fmt, bytes);
	if (!hd) return nullptr;
	Tex t; t.handle = hd; t.bytes = bytes; t.lastUse = _frame; t.opq = (fmt == FMT_1555);
	{	/* rectangle des pixels visibles de la tuile */
		int bx0 = vw, by0 = vh, bx1 = 0, by1 = 0;
		for (int y = 0; y < vh; y++) {
			const byte *row = px + (size_t)(y0 + y) * s.pitch + x0;
			for (int x = 0; x < vw; x++)
				if (useSoft ? soft[(size_t)y * vw + x] != 0 : get(x, y) != 0) { if (x < bx0) bx0 = x; if (x >= bx1) bx1 = x + 1; if (y < by0) by0 = y; if (y >= by1) by1 = y + 1; }
		}
		if (bx1 <= bx0) { t.bx0 = t.bx1 = 0; }
		else {   /* un texel de marge : le filtrage bilinéaire (mode HD) adoucit le bord avec le texel voisin */
			bx0 = std::max(0, bx0 - 1); by0 = std::max(0, by0 - 1); bx1 = std::min(vw, bx1 + 1); by1 = std::min(vh, by1 + 1);
			t.bx0 = bx0 / (float)TILE; t.by0 = by0 / (float)TILE; t.bx1 = bx1 / (float)TILE; t.by1 = by1 / (float)TILE;
		}
	}
	return &(e.tiles[idx] = t);
}

void Core::drawTiles(LayerEntry &e, const Graphics::Surface &s, const uint16 *pal, int mode, int blend,
                     int left, int top, int winW, int winH, bool wrapY) {
	const int w = s.w, h = s.h;
	const int nx = (w + TILE - 1) / TILE, ny = (h + TILE - 1) / TILE;
	const int kx1 = (left + winW - 1) / w;
	const int ky0 = wrapY ? top / h : 0, ky1 = wrapY ? (top + winH - 1) / h : 0;
	int dummy = 0;
	for (int ky = ky0; ky <= ky1; ky++)
	for (int kx = 0; kx <= kx1; kx++)
	for (int ty = 0; ty < ny; ty++)
	for (int tx = 0; tx < nx; tx++) {
		const int ux0 = kx * w + tx * TILE, ux1 = ux0 + std::min((int)TILE, w - tx * TILE);
		int ix0 = std::max(ux0, left), ix1 = std::min(ux1, left + winW);
		if (ix0 >= ix1) continue;
		const int uy0 = ky * h + ty * TILE, uy1 = uy0 + std::min((int)TILE, h - ty * TILE);
		int iy0 = std::max(uy0, top), iy1 = std::min(uy1, top + winH);
		if (iy0 >= iy1) continue;
		Tex *t = layerTile(e, tx, ty, s, pal, mode, dummy);
		if (!t) { stats.skipped++; continue; }
		if (t->bx0 >= t->bx1) continue;   /* tuile entièrement transparente */
		ix0 = std::max(ix0, ux0 + (int)(t->bx0 * TILE)); ix1 = std::min(ix1, ux0 + (int)(t->bx1 * TILE));
		iy0 = std::max(iy0, uy0 + (int)(t->by0 * TILE)); iy1 = std::min(iy1, uy0 + (int)(t->by1 * TILE));
		if (ix0 >= ix1 || iy0 >= iy1) continue;
		_opqNext = _opqLayer && t->opq;
		addQuad(t->handle, (float)(ix0 - left), (float)(iy0 - top), (float)(ix1 - left), (float)(iy1 - top),
		        (ix0 - ux0) / (float)TILE, (iy0 - uy0) / (float)TILE, (ix1 - ux0) / (float)TILE, (iy1 - uy0) / (float)TILE, blend);
		_opqNext = false;
	}
}

void Core::layer(const Graphics::Surface &s, uint32 version, const byte *palette, const Common::Rect &r, AlphaBlendMode alpha) {
#ifdef BD_SIM_DIAG
	if (BdProf::envInt("BD_OPDUMP", -1) == (int)_frame) BdProf::say("LAYER %dx%d mode %d ops %zu\n", s.w, s.h, (int)alpha, _ops.size());
#endif
	BdProf::Sub profSub(BdProf::S_LAYER);
	BdProf::Scope profPrep(BdProf::PREP);
	if (s.w <= 0 || s.h <= 0 || s.format.bytesPerPixel != 1) return;
	const int mode = modeOf(alpha);
	const int w = s.w;
	const int winW = std::min(w, (int)DRAGONS_SCREEN_WIDTH);
	const int top = std::max<int>(0, r.top);
	const int winH = std::min<int>(r.height(), s.h - top);
	if (winH <= 0) return;
	const int left = ((r.left % w) + w) % w;

	uint16 pal[256];
	memcpy(pal, palette, 512);
	_opqLayer = _ops.empty() && mode != 2;
	if (_sink->hasPal8() && mode != 2 && layerPal(s, version, pal, mode, left, top, winW, winH)) { _opqLayer = false; return; }
	auto entry = [&](int m) -> LayerEntry & {
		Key key = {{ (uint32)(uintptr_t)&s, (uint32)((uint64)(uintptr_t)&s >> 32), version, hashPalette(palette),
		             (uint32)m, (uint32)((s.w << 16) | s.h) }};
		auto it = _layers.find(key);
		if (it == _layers.end()) {
			LayerEntry e; e.w = s.w; e.h = s.h; e.lastUse = _frame;
			if (m == 2) {   /* flou seulement pour une couche presque toute additive (brume, nuages), pas pour quelques pixels */
				const byte *px = (const byte *)s.getPixels(); size_t nA = 0, nV = 0;
				for (int y = 0; y < s.h; y += 2) for (int x = 0; x < s.w; x += 2) { const uint16 c = pal[px[(size_t)y * s.pitch + x]]; if (c) { nV++; if (c & 0x8000) nA++; } }
				e.soft = nV && nA * 10 >= nV * 9;
			}
			it = _layers.insert(std::make_pair(key, e)).first;
		}
		it->second.lastUse = _frame;
		return it->second;
	};
	if (mode == 2) {
		_opqLayer = false;
		if (_sink->hasPal8() && !entry(2).soft) {
			/* textures à palette : les fondus de palette (yeux du mini-jeu des lapins, toutes les 10 images) ne recréent
			   plus toutes les tuiles des trois couches (16 Mo de textures par seconde sinon) */
			uint16 p4[256], p2[256];
			for (int c = 0; c < 256; c++) { p4[c] = (pal[c] & 0x8000) ? 0 : pal[c]; p2[c] = (pal[c] & 0x8000) ? (pal[c] & 0x7fff) : 0; }
			p4[0] = p2[0] = 0;
			if (layerPal(s, version, p4, 0, left, top, winW, winH) && layerPal(s, version, p2, 2, left, top, winW, winH)) { _opqLayer = false; return; }
		}
		drawTiles(entry(4), s, pal, 4, BL_ALPHA, left, top, winW, winH, false);
		drawTiles(entry(2), s, pal, 2, BL_ADD, left, top, winW, winH, false);
	} else
		drawTiles(entry(mode), s, pal, mode, BL_ALPHA, left, top, winW, winH, false);
	_opqLayer = false;
}

/* ---- couches à palette ---- */

int Core::paletteBank(const uint16 *pal, int mode) {
	uint32 h = 2166136261u;
	for (int i = 0; i < 256; i++) h = (h ^ pal[i]) * 16777619u;
	h ^= (uint32)mode * 0x9e3779b9u;
	for (int b = 0; b < 4; b++) if (_bankSet[b] && _bankHash[b] == h) { _bankUse[b] = _frame; return b; }
	/* banque libre ou la plus ancienne, jamais une déjà utilisée dans cette image */
	int best = -1;
	for (int b = 0; b < 4; b++) {
		if (_bankSet[b] && _bankUse[b] == _frame) continue;
		if (best < 0 || !_bankSet[b] || (_bankSet[best] && _bankUse[b] < _bankUse[best])) best = b;
	}
	if (best < 0) return -1;
	_sink->setPalette(best, pal, mode);
	_bankSet[best] = true; _bankHash[best] = h; _bankUse[best] = _frame;
	return best;
}

bool Core::layerPal(const Graphics::Surface &s, uint32 version, const uint16 *pal, int mode, int left, int top, int winW, int winH) {
	/* palettes en ARGB1555 (le format 8888 est rendu plus lentement par le PVR) : pas de demi-transparence, ces couches
	   gardent l'ancien chemin (textures 4444) */
	if (mode == 1) for (int c = 1; c < 256; c++) if (pal[c] & 0x8000) return false;
	const int bank = paletteBank(pal, mode);
	if (bank < 0) return false;
	Key key = {{ (uint32)(uintptr_t)&s, (uint32)((uint64)(uintptr_t)&s >> 32), version, 0x9a18u, 0, (uint32)((s.w << 16) | s.h) }};
	auto it = _layers.find(key);
	if (it == _layers.end()) {
		LayerEntry e; e.w = s.w; e.h = s.h; e.lastUse = _frame;
		it = _layers.insert(std::make_pair(key, e)).first;
	}
	LayerEntry &e = it->second;
	e.lastUse = _frame;
	const int w = s.w, h = s.h;
	const int nx = (w + TILE - 1) / TILE, ny = (h + TILE - 1) / TILE;
	const int kx1 = (left + winW - 1) / w;
	const byte *px = (const byte *)s.getPixels();
	for (int kx = 0; kx <= kx1; kx++)
	for (int ty = 0; ty < ny; ty++)
	for (int tx = 0; tx < nx; tx++) {
		const int ux0 = kx * w + tx * TILE, ux1 = ux0 + std::min((int)TILE, w - tx * TILE);
		int ix0 = std::max(ux0, left), ix1 = std::min(ux1, left + winW);
		if (ix0 >= ix1) continue;
		const int uy0 = ty * TILE, uy1 = uy0 + std::min((int)TILE, h - ty * TILE);
		int iy0 = std::max(uy0, top), iy1 = std::min(uy1, top + winH);
		if (iy0 >= iy1) continue;
		const int idx = ty * 64 + tx;
		auto ti = e.tiles.find(idx);
		if (ti == e.tiles.end()) {
			/* tuile : indices bruts (une seule fois), rectangle de chaque indice */
			const int vw = std::min((int)TILE, w - tx * TILE), vh = std::min((int)TILE, h - ty * TILE);
			uint8_t *ib = (uint8_t *)_stage;   /* 64 Ko sur les 256 Ko du tampon */
			memset(ib, 0, TILE * TILE);
			std::vector<uint16> box(256 * 4);
			std::vector<uint32> cells(64 * 8, 0);
			for (int k = 0; k < 256; k++) { box[k * 4] = TILE; box[k * 4 + 1] = TILE; box[k * 4 + 2] = 0; box[k * 4 + 3] = 0; }
			for (int y = 0; y < vh; y++) {
				const byte *row = px + (size_t)(ty * TILE + y) * s.pitch + tx * TILE;
				uint8_t *d = ib + y * TILE;
				for (int x = 0; x < vw; x++) {
					const byte c = row[x]; d[x] = c;
					cells[((y >> 5) * 8 + (x >> 5)) * 8 + (c >> 5)] |= 1u << (c & 31);
					uint16 *b = &box[c * 4];
					if (x < b[0]) b[0] = x; if (y < b[1]) b[1] = y; if (x + 1 > b[2]) b[2] = x + 1; if (y + 1 > b[3]) b[3] = y + 1;
				}
			}
			if (!makeRoom(TILE * TILE)) { stats.texFailed++; return false; }
			uint32 hd;
			{ BdProf::Scope prof(BdProf::TEX); hd = _sink->texCreatePal8(TILE, ib); }
			if (!hd) { stats.texFailed++; return false; }
			_used += TILE * TILE; stats.texCreated++; stats.bytesCreated += TILE * TILE;
			Tex t; t.handle = hd; t.bytes = TILE * TILE; t.lastUse = _frame;
			ti = e.tiles.insert(std::make_pair(idx, t)).first;
			e.ibox[idx] = box;
			e.cells[idx] = cells;
		}
		ti->second.lastUse = _frame;
		/* rectangle visible avec cette palette (+1 texel de marge), et pas de demi-transparence -> liste opaque possible */
		const std::vector<uint16> &box = e.ibox[idx];
		int bx0 = TILE, by0 = TILE, bx1 = 0, by1 = 0; bool semi = false;
		for (int c = 0; c < 256; c++) {
			const uint16 *b = &box[c * 4];
			if (b[2] == 0 || pal[c] == 0) continue;
			if (mode == 1 && (pal[c] & 0x8000)) semi = true;
			bx0 = std::min<int>(bx0, b[0]); by0 = std::min<int>(by0, b[1]); bx1 = std::max<int>(bx1, b[2]); by1 = std::max<int>(by1, b[3]);
		}
		if (bx1 <= bx0) continue;
		bx0 = std::max(0, bx0 - 1); by0 = std::max(0, by0 - 1); bx1 = std::min((int)TILE, bx1 + 1); by1 = std::min((int)TILE, by1 + 1);
		ix0 = std::max(ix0, ux0 + bx0); ix1 = std::min(ix1, ux0 + bx1);
		iy0 = std::max(iy0, uy0 + by0); iy1 = std::min(iy1, uy0 + by1);
		if (ix0 >= ix1 || iy0 >= iy1) continue;
		_opqNext = _opqLayer && !semi && mode != 2;
		_bankNext = bank;
		/* cases 32×32 contenant au moins une couleur visible : une couche presque vide avec cette palette (neige qui tombe,
		   eau…) n'envoie que ses cases utiles, par bandes horizontales */
		uint32 vis[8] = {0, 0, 0, 0, 0, 0, 0, 0};
		for (int c = 1; c < 256; c++) if (pal[c]) vis[c >> 5] |= 1u << (c & 31);
		const std::vector<uint32> &cl = e.cells[idx];
		bool cellHit[64]; int nHit = 0;
		for (int k = 0; k < 64; k++) {
			bool h = false;
			for (int w8 = 0; w8 < 8 && !h; w8++) h = (cl[k * 8 + w8] & vis[w8]) != 0;
			cellHit[k] = h; nHit += h;
		}
		const int blendL = mode == 2 ? BL_ADD : BL_ALPHA;
		auto emit = [&](int a0, int b0, int a1, int b1) {
			a0 = std::max(a0, ix0); a1 = std::min(a1, ix1); b0 = std::max(b0, iy0); b1 = std::min(b1, iy1);
			if (a0 >= a1 || b0 >= b1) return;
			_opqNext = _opqLayer && !semi && mode != 2; _bankNext = bank;
			addQuad(ti->second.handle, (float)(a0 - left), (float)(b0 - top), (float)(a1 - left), (float)(b1 - top),
			        (a0 - ux0) / (float)TILE, (b0 - uy0) / (float)TILE, (a1 - ux0) / (float)TILE, (b1 - uy0) / (float)TILE, blendL);
		};
		if (nHit >= 40) emit(ix0, iy0, ix1, iy1);   /* presque pleine : un seul quad */
		else for (int cy = 0; cy < 8; cy++) {
			for (int cx = 0; cx < 8; cx++) {
				if (!cellHit[cy * 8 + cx]) continue;
				int cx1 = cx; while (cx1 + 1 < 8 && cellHit[cy * 8 + cx1 + 1]) cx1++;
				/* un texel de marge (filtrage) */
				{ const int mg = mode == 2 ? 0 : 1;   /* additif : pas de recouvrement entre bandes (il doublerait l'ajout : traits clairs) */
				  emit(ux0 + cx * 32 - mg, uy0 + cy * 32 - mg, ux0 + (cx1 + 1) * 32 + mg, uy0 + (cy + 1) * 32 + mg); }
				cx = cx1;
			}
		}
		_opqNext = false; _bankNext = -1;
	}
	return true;
}

/* ---- décors HD : tuiles VQ de 256x256 texels = 128x128 pixels d'origine ---- */

/* Rectangle visible d'une tuile VQ (au bloc 2x2 près, plus un bloc de marge pour le filtrage bilinéaire).
   Carte d'indices twiddlée : bits pairs = y du bloc, bits impairs = x (voir tools/hd_pack.py). */
static inline int unspread(uint32 v) { int r = 0; for (int b = 0; b < 8; b++) r |= ((v >> (2 * b)) & 1) << b; return r; }
void Core::vqBounds(const byte *buf, int fmt, Tex &t) {
#ifdef BD_SIM_DIAG
	if (BdProf::envFlag("BD_NOVQB")) return;
#endif
	if (fmt != 1 && fmt != 2) { t.iu0 = t.iv0 = 0; t.iu1 = t.iv1 = 1; return; }   /* RGB565 : opaque */
	bool vis[256], full[256];
	for (int c = 0; c < 256; c++) {
		const uint16 *cb = (const uint16 *)(buf + c * 8);
		bool v = false, f = true;
		for (int k = 0; k < 4; k++) {
			v |= fmt == 1 ? (cb[k] & 0x8000) != 0 : (cb[k] & 0xf000) != 0;
			f &= fmt == 1 ? (cb[k] & 0x8000) != 0 : (cb[k] & 0xf000) == 0xf000;
		}
		vis[c] = v; full[c] = f;
	}
	{	/* rectangle entièrement opaque (au bloc près, un bloc de marge) */
		const int NB = HD_TILE / 2;
		static byte fullAt[(HD_TILE / 2) * (HD_TILE / 2)];
		for (int k = 0; k < NB * NB; k++) fullAt[unspread((uint32)k) * NB + unspread((uint32)k >> 1)] = full[buf[2048 + k]];
		int a0, b0, a1, b1;
		if (largestRect(NB, NB, [&](int x, int y) { return fullAt[y * NB + x] != 0; }, a0, b0, a1, b1) > 0) {
			t.iu0 = (a0 + 1) / (float)NB; t.iv0 = (b0 + 1) / (float)NB; t.iu1 = (a1 - 1) / (float)NB; t.iv1 = (b1 - 1) / (float)NB;
		}
	}
	bool allFull = true;
	for (int k = 0; k < (HD_TILE / 2) * (HD_TILE / 2); k++) allFull &= full[buf[2048 + k]];
	t.opq = allFull;   /* aucun texel transparent : la tuile peut aller dans la liste opaque */
	const int N = HD_TILE / 2;
	int x0 = N, y0 = N, x1 = 0, y1 = 0;
	const byte *idx = buf + 2048;
	for (int k = 0; k < N * N; k++) {
		if (!vis[idx[k]]) continue;
		const int bx = unspread((uint32)k >> 1), by = unspread((uint32)k);
		if (bx < x0) x0 = bx; if (bx >= x1) x1 = bx + 1; if (by < y0) y0 = by; if (by >= y1) y1 = by + 1;
	}
	if (x1 <= x0) { t.bx0 = t.bx1 = 0; return; }
	x0 = std::max(0, x0 - 1); y0 = std::max(0, y0 - 1); x1 = std::min(N, x1 + 1); y1 = std::min(N, y1 + 1);
	t.bx0 = x0 / (float)N; t.by0 = y0 / (float)N; t.bx1 = x1 / (float)N; t.by1 = y1 / (float)N;
}

bool Core::hdLoad(HdLayerEntry &h) {
	/* Tuile par tuile, avec un seul tampon de 18 Ko : lire la couche entière d'un coup demandait 110 Ko (640x480)
	   à plus de 700 Ko (décors larges) de mémoire contiguë, souvent indisponibles juste après un changement de salle. */
	const HdEntry &e = *h.e;
	const int n = e.tilesX * e.tilesY;
	byte *buf = (byte *)BD_ALIGNED_ALLOC(HD_TILE_BYTES);
	if (!buf) return false;
	bool ok = true;
	for (int i = 0; ok && i < n; i++) {
		if (!_hd->readTile(e, i, buf)) { ok = false; break; }
		if (!makeRoom(HD_TILE_BYTES)) { ok = false; stats.texFailed++; break; }
		uint32 hd;
		{ BdProf::Scope prof(BdProf::TEX); hd = _sink->texCreateVQ(HD_TILE, e.fmt, buf); }
		if (!hd) { ok = false; stats.texFailed++; break; }
		Tex t; t.handle = hd; t.bytes = (size_t)HD_TILE_BYTES; t.lastUse = _frame;
		t.opq = (e.fmt == 0);   /* RGB565 : aucun texel transparent */
		vqBounds(buf, e.fmt, t);
		h.l.tiles[i] = t;
		_used += HD_TILE_BYTES;
		stats.texCreated++;
		stats.bytesCreated += HD_TILE_BYTES;
	}
	free(buf);
	if (!ok) {   /* tout ou rien : une couche HD à moitié chargée serait pire que l'original */
		for (auto &t : h.l.tiles) releaseTex(t.second);
		h.l.tiles.clear();
	}
	return ok;
}

bool Core::hdLayer(const char *scene, int layerNum, const Common::Rect &r, AlphaBlendMode alpha) {
	BdProf::Sub profSub(BdProf::S_HDLAYER);
	if (!_hd || !_hd->isOpen()) return false;
	const uint32 key = ((uint32)(byte)scene[0] | ((uint32)(byte)scene[1] << 8) | ((uint32)(byte)scene[2] << 16) | ((uint32)(byte)scene[3] << 24)) ^ ((uint32)layerNum * 0x10000000u);
	auto it = _hdLayers.find(key);
	if (it == _hdLayers.end()) {
		HdLayerEntry h;
		h.e = _hd->find(scene, layerNum);
		h.failed = (h.e == nullptr); h.retryAt = 0; h.tries = 0;
		h.l.w = h.e ? h.e->w / 2 : 0; h.l.h = h.e ? h.e->oh : 0; h.l.lastUse = _frame;
		it = _hdLayers.insert(std::make_pair(key, h)).first;
	}
	HdLayerEntry &h = it->second;
	h.l.lastUse = _frame;
	if (h.failed) return false;
	if (h.retryAt > _frame) return false;
	/* Couche 4444 en mode NONE : l'original dessine les demi-transparences (noir 0x8000) pleines ; en HD elles restent a moitie
	   transparentes. Sur un fond noir (couche 0) c'est identique ; ailleurs l'ecart ne touche que les pixels d'ombre. */
	/* Échec de chargement (mémoire ou VRAM momentanément pleines, typiquement au changement de salle) : pas définitif,
	   on réessaie 2 images plus tard ; abandon seulement après de nombreux essais. */
	/* Tuiles évincées (mémoire vidéo pleine, ex. pendant une animation de personnages) : la couche était dessinée avec un
	   trou noir. On recharge la couche entière si une tuile manque. */
	if (!h.l.tiles.empty() && (int)h.l.tiles.size() < h.e->tilesX * h.e->tilesY) {
		for (auto &t : h.l.tiles) releaseTex(t.second);
		h.l.tiles.clear();
	}
	if (h.l.tiles.empty() && !hdLoad(h)) { if (++h.tries >= 120) h.failed = true; else h.retryAt = _frame + 2; stats.hdLoadRetry++; return false; }

	const int w = h.l.w, hh = h.l.h;               /* pixels d'origine */
	const int winW = std::min(w, (int)DRAGONS_SCREEN_WIDTH);
	const int top = std::max<int>(0, r.top);
	const int winH = std::min<int>(r.height(), hh - top);
	if (winH <= 0) return true;
	const int left = ((r.left % w) + w) % w;
	const int T = HD_TILE / 2;                      /* une tuile = 128 pixels d'origine */
	const int nx = h.e->tilesX, ny = h.e->tilesY;
	const int kx1 = (left + winW - 1) / w;
	const int blend = (alpha == Dragons::NORMAL || alpha == Dragons::NONE) ? BL_ALPHA : BL_ADD;
	const bool opqLayer = _ops.empty() && blend == BL_ALPHA;
	for (int kx = 0; kx <= kx1; kx++)
	for (int ty = 0; ty < ny; ty++)
	for (int tx = 0; tx < nx; tx++) {
		const int ux0 = kx * w + tx * T, ux1 = ux0 + std::min(T, w - tx * T);
		int ix0 = std::max(ux0, left), ix1 = std::min(ux1, left + winW);
		if (ix0 >= ix1) continue;
		const float Ty = (float)HD_TILE * hh / h.e->h;   /* pixels d'origine par tuile en hauteur (128 en x2, 106,7 en 480 lignes) */
		const float uy0 = ty * Ty, uy1 = std::min(uy0 + Ty, (float)hh);
		float iy0 = std::max(uy0, (float)top), iy1 = std::min(uy1, (float)(top + winH));
		if (iy0 >= iy1) continue;
		auto ti = h.l.tiles.find(ty * nx + tx);
		if (ti == h.l.tiles.end()) continue;
		ti->second.lastUse = _frame;
		const Tex &tt = ti->second;
		if (tt.bx0 >= tt.bx1) continue;   /* tuile entièrement transparente */
		ix0 = std::max(ix0, ux0 + (int)(tt.bx0 * T)); ix1 = std::min(ix1, ux0 + (int)std::ceil(tt.bx1 * T));
		iy0 = std::max(iy0, std::floor(uy0 + tt.by0 * Ty)); iy1 = std::min(iy1, std::ceil(uy0 + tt.by1 * Ty));   /* bords sur des pixels entiers */
		if (ix0 >= ix1 || iy0 >= iy1) continue;
		/* Coordonnées de texture rentrées d'un demi-texel aux bords de tuile : sinon le filtrage bilinéaire de la PVR
		   mélange la ligne du bord avec celle d'en face (répétition) et trace un trait à chaque jonction (y = 256 à l'écran). */
		const float e = 0.5f / HD_TILE;
		auto cl = [e](float v) { return v < e ? e : (v > 1.0f - e ? 1.0f - e : v); };
		_opqNext = opqLayer && tt.opq;
		_fullNext = tt.opq;
		if (tt.iu0 < tt.iu1) { _occNext = true; _occ[0] = tt.iu0; _occ[1] = tt.iv0; _occ[2] = tt.iu1; _occ[3] = tt.iv1; }
		addQuad(ti->second.handle, (float)(ix0 - left), iy0 - top, (float)(ix1 - left), iy1 - top,
		        cl((ix0 - ux0) / (float)T), cl((iy0 - uy0) / Ty), cl((ix1 - ux0) / (float)T), cl((iy1 - uy0) / Ty), blend);
		_fullNext = false;
		_opqNext = false;
	}
	return true;
}

bool Core::hdPatch(const char *name, int layerNum, int x, int y, bool draw) {
	if (!_hd || !_hd->isOpen()) return false;
	uint32 key = 2166136261u;   /* nom jusqu'à 8 caractères (« r038s01b ») ; bit 31 : jamais égal à une clé de couche (4 lettres) */
	for (int i = 0; i < 8 && name[i]; i++) key = (key ^ (byte)name[i]) * 16777619u;
	key = ((key ^ ((uint32)layerNum * 0x9e3779b9u)) | 0x80000000u) & ~0x00000000u;
	auto it = _hdLayers.find(key);
	if (it == _hdLayers.end()) {
		HdLayerEntry h;
		h.e = _hd->find(name, layerNum);
		h.failed = (h.e == nullptr); h.retryAt = 0; h.tries = 0;
		h.l.w = h.e ? h.e->w / 2 : 0; h.l.h = h.e ? h.e->oh : 0; h.l.lastUse = _frame;
		it = _hdLayers.insert(std::make_pair(key, h)).first;
	}
	HdLayerEntry &h = it->second;
	h.l.lastUse = _frame;
	if (h.failed || h.retryAt > _frame) return false;
	/* Tuiles évincées (mémoire vidéo pleine, ex. pendant une animation de personnages) : la couche était dessinée avec un
	   trou noir. On recharge la couche entière si une tuile manque. */
	if (!h.l.tiles.empty() && (int)h.l.tiles.size() < h.e->tilesX * h.e->tilesY) {
		for (auto &t : h.l.tiles) releaseTex(t.second);
		h.l.tiles.clear();
	}
	if (h.l.tiles.empty() && !hdLoad(h)) { if (++h.tries >= 120) h.failed = true; else h.retryAt = _frame + 2; stats.hdLoadRetry++; return false; }
	if (!draw) return true;
	const int T = HD_TILE / 2;
	const float Ty = (float)HD_TILE * h.l.h / h.e->h;
	const float e = 0.5f / HD_TILE;
	auto cl = [e](float v) { return v < e ? e : (v > 1.0f - e ? 1.0f - e : v); };
	for (int ty = 0; ty < h.e->tilesY; ty++)
	for (int tx = 0; tx < h.e->tilesX; tx++) {
		auto ti = h.l.tiles.find(ty * h.e->tilesX + tx);
		if (ti == h.l.tiles.end()) continue;
		ti->second.lastUse = _frame;
		const float ux1 = (float)std::min(T, h.l.w - tx * T), uy0 = ty * Ty, uy1 = std::min(uy0 + Ty, (float)h.l.h);
		if (ux1 <= 0 || uy1 <= uy0) continue;
		const Tex &tt = ti->second;
		if (tt.bx0 >= tt.bx1) continue;
		const float a0 = std::floor(tt.bx0 * T), a1 = std::min(ux1, std::ceil(tt.bx1 * T));
		const float b0 = std::max(0.0f, std::floor(uy0 + tt.by0 * Ty) - uy0), b1 = std::min(uy1 - uy0, std::ceil(uy0 + tt.by1 * Ty) - uy0);
		if (a1 <= a0 || b1 <= b0) continue;
		if (tt.iu0 < tt.iu1) { _occNext = true; _occ[0] = tt.iu0; _occ[1] = tt.iv0; _occ[2] = tt.iu1; _occ[3] = tt.iv1; }
		addQuad(ti->second.handle, x + tx * T + a0, y + uy0 + b0, x + tx * T + a1, y + uy0 + b1,
		        cl(a0 / T), cl(b0 / Ty), cl(a1 / T), cl(b1 / Ty), BL_ALPHA);
	}
	return true;
}

/* ---- sprites HD : morceaux de trames dans des pages VQ de 256x256 ---- */


bool Core::hdSprite(int actorFile, uint16 frameOffset, int paletteKind, int x, int y, bool flipX, AlphaBlendMode alpha, uint16 scale) {
	BdProf::Sub profSub(BdProf::S_HDSPRITE);
	if (!_hdSpr || !_hdSpr->isOpen()) { g_hdSprWhy = 1; return false; }
	if (alpha != Dragons::NONE && alpha != Dragons::NORMAL) { g_hdSprWhy = 2; return false; }
	const HdSprFrame *f = _hdSpr->find(actorFile, frameOffset, paletteKind);
	/* trame d'une autre version du jeu (disque USA : HDSPR.DAT fait d'après la version France) : tailles différentes -> original */
	if (f && g_hdSprExpectW && (f->w != g_hdSprExpectW || f->h != g_hdSprExpectH)) { g_hdSprWhy = 8; return false; }
	if (!f || f->w == 0 || f->h == 0) { g_hdSprWhy = 3; stats.hdSprMissNoFrame++; if (actorFile >= 0 && actorFile < 1024) { stats.missByActor[actorFile]++; stats.missKinds[actorFile] |= 1 << (paletteKind & 7); } return false; }
	/* toutes les pages de la trame doivent être là, sinon l'original (jamais une trame à moitié HD).
	   Les pages manquantes de la trame sont lues d'un seul coup (pages consécutives : un seul déplacement de la tête, ~120 ms sur GD-ROM). */
	const int PS = _hdSpr->pageSize();
	const uint32 PB = _hdSpr->pageBytes();
	uint32 lo = 0xffffffff, hi = 0;
	for (int i = 0; i < f->nPieces; i++) {
		const uint32 page = _hdSpr->piece(f, i).page;
		auto it = _hdPages.find(page);
		if (it != _hdPages.end()) { it->second.lastUse = _frame; continue; }
		if (_hdPagesFailed.count(page)) { g_hdSprWhy = 4; return false; }
		lo = std::min(lo, page); hi = std::max(hi, page);
	}
	if (lo <= hi) {
		uint32 n = hi - lo + 1;
		byte *buf = n <= 32 ? (byte *)BD_ALIGNED_ALLOC((size_t)n * PB) : nullptr;
		bool grouped = buf != nullptr && _hdSpr->readPages(lo, (int)n, buf);
		if (!grouped) { free(buf); buf = (byte *)BD_ALIGNED_ALLOC(PB); n = 0; }
		if (!buf) { g_hdSprWhy = 5; return false; }
		for (int i = 0; i < f->nPieces; i++) {
			const HdSprPiece &pc = _hdSpr->piece(f, i);
			if (_hdPages.count(pc.page)) continue;
			const byte *src = buf;
			bool ok = true;
			if (grouped) src = buf + (size_t)(pc.page - lo) * PB;
			else ok = _hdSpr->readPage(pc.page, buf);
			ok = ok && pc.fmt <= 2 && makeRoom(PB);
			uint32 h = 0;
			if (ok) { BdProf::Scope prof(BdProf::TEX); h = _sink->texCreateVQ(PS, pc.fmt, src); }
			if (!h) { g_hdSprWhy = ok ? 6 : 7; stats.texFailed++; if (!ok) _hdPagesFailed.insert(pc.page); free(buf); return false; }
			Tex t = { h, (size_t)PB, _frame };
			_hdPages[pc.page] = t;
			_used += PB; stats.texCreated++; stats.bytesCreated += PB;
		}
		free(buf);
	}
	const float dw = (float)(f->w * scale) / (float)DRAGONS_ENGINE_SPRITE_100_PERCENT_SCALE;
	const float dh = (float)(f->h * scale) / (float)DRAGONS_ENGINE_SPRITE_100_PERCENT_SCALE;
	const float sx = dw / (2.0f * f->w), sy = dh / (2.0f * f->h);   /* pixels d'origine par texel HD */
	for (int i = 0; i < f->nPieces; i++) {
		const HdSprPiece &p = _hdSpr->piece(f, i);
		Tex &t = _hdPages[p.page];
		float x0 = x + p.dx * sx, x1 = x + (p.dx + p.w) * sx;
		const float y0 = y + p.dy * sy, y1 = y + (p.dy + p.h) * sy;
		const float inv = 1.0f / PS;
		float u0 = p.x * inv, u1 = (p.x + p.w) * inv;
		if (flipX) { x0 = x + dw - (p.dx + p.w) * sx; x1 = x + dw - p.dx * sx; std::swap(u0, u1); }
		addQuad(t.handle, x0, y0, x1, y1, u0, p.y * inv, u1, (p.y + p.h) * inv, BL_ALPHA);
	}
	stats.hdSprHit++;
	return true;
}

void Core::layerWrappedY(const Graphics::Surface &s, const byte *palette, int yOffset) {
	BdProf::Sub profSub(BdProf::S_WRAPY);
	BdProf::Scope profPrep(BdProf::PREP);
	if (s.w <= 0 || s.h <= 0 || s.format.bytesPerPixel != 1) return;
	const byte *px = (const byte *)s.getPixels();
	uint32 h1 = 2166136261u, h2 = 0x9e3779b9u;
	for (int j = 0; j < s.h; j++) hashBytes(px + j * s.pitch, s.w, h1, h2);
	Key key = {{ h1, h2, (uint32)((s.w << 16) | s.h), hashPalette(palette), 0xC0DE, 1 }};
	auto it = _layers.find(key);
	if (it == _layers.end()) {
		LayerEntry e; e.w = s.w; e.h = s.h; e.lastUse = _frame;
		it = _layers.insert(std::make_pair(key, e)).first;
	}
	it->second.lastUse = _frame;
	uint16 pal[256];
	memcpy(pal, palette, 512);
	const int top = ((yOffset % s.h) + s.h) % s.h;
	drawTiles(it->second, s, pal, 0, BL_ALPHA, 0, top, std::min<int>(s.w, DRAGONS_SCREEN_WIDTH), DRAGONS_SCREEN_HEIGHT, true);
}

/* ---- fin d'image ---- */

static bool clipOp(float &x0, float &y0, float &x1, float &y1, float &u0, float &v0, float &u1, float &v1, bool solid) {
	const float cx0 = 0, cy0 = 0, cx1 = DRAGONS_SCREEN_WIDTH, cy1 = DRAGONS_SCREEN_HEIGHT;
	const float nx0 = std::max(x0, cx0), nx1 = std::min(x1, cx1), ny0 = std::max(y0, cy0), ny1 = std::min(y1, cy1);
	if (nx0 >= nx1 || ny0 >= ny1) return false;
	if (!solid) {
		const float dx = x1 - x0, dy = y1 - y0;
		const float nu0 = u0 + (u1 - u0) * (nx0 - x0) / dx, nu1 = u0 + (u1 - u0) * (nx1 - x0) / dx;
		const float nv0 = v0 + (v1 - v0) * (ny0 - y0) / dy, nv1 = v0 + (v1 - v0) * (ny1 - y0) / dy;
		u0 = nu0; u1 = nu1; v0 = nv0; v1 = nv1;
	}
	x0 = nx0; x1 = nx1; y0 = ny0; y1 = ny1;
	return true;
}

void Core::present(int shakeX, int shakeY) {
	BdProf::Scope prof(BdProf::PRESENT);
	occludeOps();
	promoteOpaque();
	_sink->beginFrame();
	bool anyOpq = false;
	for (const Op &o : _ops) anyOpq |= o.opq;
#ifdef BD_SIM_DIAG
	if (BdProf::envFlag("BD_NOOPQ")) { for (Op &o : _ops) o.opq = false; anyOpq = false; }
#endif
	for (int pass = anyOpq ? 0 : 1; pass < 2; pass++) {
	_sink->opaqueList(pass == 0);
	for (const Op &o : _ops) {
		if (o.opq != (pass == 0)) continue;
		float x0 = o.x0 + shakeX, x1 = o.x1 + shakeX, y0 = o.y0 + shakeY, y1 = o.y1 + shakeY;
		float u0 = o.u0, v0 = o.v0, u1 = o.u1, v1 = o.v1;
		if (!clipOp(x0, y0, x1, y1, u0, v0, u1, v1, o.solid)) continue;
		if (o.solid) _sink->solid(x0, y0, x1, y1, o.col);
		else if (o.bank >= 0) _sink->quadPal(o.tex, x0, y0, x1, y1, u0, v0, u1, v1, o.blend, o.bank);
		else _sink->quad(o.tex, x0, y0, x1, y1, u0, v0, u1, v1, o.blend, o.alpha);
		stats.quads++;
		stats.area += (x1 - x0) * (y1 - y0);
		if (o.opq) stats.areaOpq += (x1 - x0) * (y1 - y0);
#ifdef BD_SIM_DIAG
		if (BdProf::envInt("BD_OPDUMP", -1) == (int)_frame) BdProf::say("OP tex %u opq %d solid %d blend %d %.0f,%.0f-%.0f,%.0f occ %.0f,%.0f-%.0f,%.0f\n", o.tex, o.opq, o.solid, o.blend, x0, y0, x1, y1, o.ox0, o.oy0, o.ox1, o.oy1);
#endif
	}
	}
	_sink->endFrame();
	_frame++;

	/* libérations différées : le GPU peut lire une texture pendant 2 images après son dernier dessin */
	size_t k = 0;
	for (size_t i = 0; i < _pending.size(); i++) {
		if (_pending[i].frame + 2 <= _frame) { _sink->texDestroy(_pending[i].handle); _used -= _pending[i].bytes; }
		else _pending[k++] = _pending[i];
	}
	_pending.resize(k);

	for (auto it = _hdLayers.begin(); it != _hdLayers.end();) {
		if (it->second.l.lastUse + 2 <= _frame) {
			for (auto &t : it->second.l.tiles) releaseTex(t.second);
			it = _hdLayers.erase(it);
		} else ++it;
	}

	/* décors plus utilisés depuis 2 images (changement de scène, de palette) */
	for (auto it = _layers.begin(); it != _layers.end();) {
		if (it->second.lastUse + 2 <= _frame) {
			for (auto &t : it->second.tiles) releaseTex(t.second);
			it = _layers.erase(it);
		} else ++it;
	}
}

} // namespace Gpu
