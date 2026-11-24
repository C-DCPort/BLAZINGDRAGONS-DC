/* Manette Dreamcast dessinée pour l'écran « Contrôles ».
   Formes décrites par des fonctions de distance signée (SDF) : bords lissés sans suréchantillonnage. */
#include "padart.h"
#include "inputmap.h"
#include <math.h>

namespace PadArt {

static inline uint16_t rgb(int r, int g, int b) { return (uint16_t)(0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)); }

struct Col { float r, g, b; };
static inline Col C(int r, int g, int b) { return { (float)r, (float)g, (float)b }; }

/* ---- distances signées (négatives à l'intérieur) ---- */
static inline float sdCircle(float x, float y, float cx, float cy, float r) { return sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) - r; }
static inline float sdRBox(float x, float y, float cx, float cy, float hw, float hh, float r) {
	float qx = fabsf(x - cx) - hw + r, qy = fabsf(y - cy) - hh + r;
	float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
	float in = qx > qy ? qx : qy; if (in > 0) in = 0;
	return sqrtf(ox * ox + oy * oy) + in - r;
}
static inline float sdCapsule(float x, float y, float ax, float ay, float bx, float by, float r) {
	float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay;
	float h = (px * dx + py * dy) / (dx * dx + dy * dy); h = h < 0 ? 0 : (h > 1 ? 1 : h);
	float ex = px - dx * h, ey = py - dy * h;
	return sqrtf(ex * ex + ey * ey) - r;
}
static inline float smin(float a, float b, float k) {
	float h = 0.5f + 0.5f * (b - a) / k; h = h < 0 ? 0 : (h > 1 ? 1 : h);
	return b + (a - b) * h - k * h * (1 - h);
}

/* ---- toile RGB555 avec mélange ---- */
struct Canvas {
	uint16_t *px; int pitch;
	void blend(int x, int y, Col c, float a) {
		if (x < 0 || y < 0 || x >= W || y >= H || a <= 0) return;
		uint16_t &d = px[y * pitch + x];
		if (a >= 1) { d = rgb((int)c.r, (int)c.g, (int)c.b); return; }
		float r = ((d >> 10) & 31) * 8.225f, g = ((d >> 5) & 31) * 8.225f, b = (d & 31) * 8.225f;
		d = rgb((int)(r + (c.r - r) * a), (int)(g + (c.g - g) * a), (int)(b + (c.b - b) * a));
	}
	/* remplit la forme sdf sur la boîte [x0,x1]x[y0,y1] ; shade(x,y,d) donne la couleur */
	template<class F, class S> void fill(int x0, int y0, int x1, int y1, F sdf, S shade) {
		for (int y = y0; y <= y1; y++)
			for (int x = x0; x <= x1; x++) {
				float fx = x + 0.5f, fy = y + 0.5f;
				float d = sdf(fx, fy);
				float a = 0.5f - d; if (a <= 0) continue; if (a > 1) a = 1;
				blend(x, y, shade(fx, fy, d), a);
			}
	}
	template<class F> void flat(int x0, int y0, int x1, int y1, F sdf, Col c) { fill(x0, y0, x1, y1, sdf, [&](float, float, float) { return c; }); }
};

/* Lettres 5x7 : A B X Y L R */
static const uint8_t kGlyph[6][7] = {
	{ 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 },
	{ 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E },
	{ 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 },
	{ 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 },
	{ 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F },
	{ 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 },
};
static void glyph(Canvas &c, int idx, int cx, int cy, Col col) {
	for (int y = 0; y < 7; y++) for (int x = 0; x < 5; x++) if (kGlyph[idx][y] & (0x10 >> x)) c.blend(cx - 2 + x, cy - 3 + y, col, 1);
}

uint16_t fnColour(int f) {
	switch (f) {
	case 0: return rgb(60, 220, 90);      /* action */
	case 1: return rgb(255, 170, 30);     /* inventaire */
	case 2: return rgb(70, 210, 255);     /* défiler vers le haut */
	default: return rgb(240, 90, 220);    /* défiler vers le bas */
	}
}
static Col fnCol(int f, bool white) {
	if (white) return C(255, 255, 255);
	uint16_t v = fnColour(f);
	return C(((v >> 10) & 31) * 8, ((v >> 5) & 31) * 8, (v & 31) * 8);
}

/* silhouette du corps : bloc supérieur + deux poignées, fondus ensemble */
static inline float sdBody(float x, float y) {
	float ex = (x - 160) / 92, ey = (y - 70) / 36;
	float dome = (sqrtf(ex * ex + ey * ey) - 1) * 34;      /* dessus bombé */
	float top = smin(sdRBox(x, y, 160, 68, 84, 22, 20), dome, 10);
	float lg = sdCapsule(x, y, 100, 76, 88, 102, 21);
	float rg = sdCapsule(x, y, 220, 76, 232, 102, 21);
	return smin(smin(top, lg, 14), rg, 14);
}

static bool s_noBg = false;   /* drawSmall : fond transparent, pas de légende */

void draw(uint16_t *px, int pitchPx, const uint32_t *assign, int nFn, int sel, int blink) {
	Canvas c = { px, pitchPx };
	for (int y = 0; y < H; y++) { uint16_t col = s_noBg ? 0 : rgb(14, 18 + y / 8, 44 + y / 6); for (int x = 0; x < W; x++) px[y * pitchPx + x] = col; }

	auto fnOf = [&](uint32_t bit) { for (int f = 0; f < nFn; f++) if (assign[f] == bit) return f; return -1; };

	/* ---- gâchettes (derrière le corps, elles dépassent en haut) ---- */
	const int trgX[2] = { 108, 212 };
	const uint32_t trgBit[2] = { InputMap::BTN_L, InputMap::BTN_R };
	for (int t = 0; t < 2; t++) {
		float cx = (float)trgX[t];
		int f = fnOf(trgBit[t]);
		if (f >= 0)
			c.flat((int)cx - 34, 12, (int)cx + 34, 46, [&](float x, float y) { return sdRBox(x, y, cx, 30, 30, 15, 11); }, fnCol(f, f == sel && blink));
		c.fill((int)cx - 30, 14, (int)cx + 30, 46, [&](float x, float y) { return sdRBox(x, y, cx, 30, 26, 12, 9); },
			[&](float, float y, float d) { float k = d > -1.5f ? 0.55f : 0.85f + 0.15f * (30 - y) / 12; return C((int)(178 * k), (int)(180 * k), (int)(190 * k)); });
	}

	/* ---- corps : gris clair, ombré vers les bords et vers le bas, liseré foncé ---- */
	c.fill(50, 28, 270, 126, sdBody, [](float, float y, float d) {
		if (d > -1.4f) return C(70, 72, 86);                    /* contour */
		float edge = d > -7 ? (7 + d) / 7 : 0;                  /* assombrit près du bord */
		float k = 1.0f - 0.22f * edge - 0.12f * (y - 37) / 90;
		if (d < -3 && d > -5 && y < 50) k += 0.08f;            /* reflet sur l'arête supérieure */
		return C((int)(232 * k), (int)(233 * k), (int)(238 * k));
	});

	/* ---- emplacement VMU au centre, avec la VMU et son écran ---- */
	c.flat(130, 34, 190, 84, [](float x, float y) { return sdRBox(x, y, 160, 58, 26, 23, 6); }, C(64, 66, 78));
	c.fill(134, 38, 186, 82, [](float x, float y) { return sdRBox(x, y, 160, 59, 21, 19, 5); },
		[](float, float y, float d) { float k = d > -1.2f ? 0.75f : 1.0f - 0.15f * (y - 40) / 40; return C((int)(214 * k), (int)(214 * k), (int)(220 * k)); });
	c.flat(144, 44, 176, 66, [](float x, float y) { return sdRBox(x, y, 160, 54, 13, 9, 2); }, C(60, 64, 60));
	c.flat(146, 46, 174, 64, [](float x, float y) { return sdRBox(x, y, 160, 54, 11, 7, 1); }, C(150, 176, 132));
	c.flat(150, 68, 158, 76, [](float x, float y) { return sdCircle(x, y, 153, 72, 3); }, C(120, 122, 132));
	c.flat(162, 68, 172, 76, [](float x, float y) { return sdCircle(x, y, 168, 72, 3); }, C(120, 122, 132));

	/* ---- stick analogique (en haut à gauche) ---- */
	c.flat(84, 42, 120, 78, [](float x, float y) { return sdCircle(x, y, 102, 60, 15); }, C(150, 152, 164));
	c.fill(86, 44, 118, 76, [](float x, float y) { return sdCircle(x, y, 102, 60, 12.5f); },
		[](float x, float y, float d) { float l = 1.0f - 0.35f * ((x - 98) * (x - 98) + (y - 56) * (y - 56)) / 230; if (d > -1.2f) l = 0.5f; return C((int)(120 * l), (int)(122 * l), (int)(136 * l)); });
	c.flat(94, 52, 110, 68, [](float x, float y) { return sdCircle(x, y, 102, 60, 6); }, C(96, 98, 112));

	/* ---- croix directionnelle (en bas à gauche) ---- */
	c.flat(100, 80, 132, 112, [](float x, float y) { return sdCircle(x, y, 116, 96, 15); }, C(196, 198, 206));
	auto cross = [](float x, float y) { float a = sdRBox(x, y, 116, 96, 12, 4.5f, 1.5f), b = sdRBox(x, y, 116, 96, 4.5f, 12, 1.5f); return a < b ? a : b; };
	c.fill(100, 80, 132, 112, cross, [&](float x, float y, float d) { return d > -1.0f ? C(30, 30, 38) : (y < 95 || x < 115 ? C(80, 82, 94) : C(58, 60, 70)); });

	/* ---- Start (en bas au centre) ---- */
	c.flat(150, 92, 170, 108, [](float x, float y) { return sdRBox(x, y, 160, 99, 7, 4, 3.5f); }, C(64, 66, 78));
	c.flat(152, 94, 168, 106, [](float x, float y) { return sdRBox(x, y, 160, 99, 5.5f, 2.6f, 2.5f); }, C(110, 112, 126));

	/* ---- A B X Y en losange (couleurs Dreamcast : A rouge, B bleu, X jaune, Y vert) ---- */
	struct Btn { float cx, cy; uint32_t bit; int g; Col col; } btn[4] = {
		{ 216, 54, InputMap::BTN_Y, 3, C(70, 175, 85) },
		{ 216, 84, InputMap::BTN_A, 0, C(215, 55, 55) },
		{ 201, 69, InputMap::BTN_X, 2, C(235, 195, 45) },
		{ 231, 69, InputMap::BTN_B, 1, C(60, 105, 215) },
	};
	for (auto &b : btn) {   /* anneaux de couleur d'abord, pour ne pas recouvrir un bouton voisin */
		int f = fnOf(b.bit);
		if (f < 0) continue;
		Col rc = fnCol(f, f == sel && blink);
		c.flat((int)b.cx - 12, (int)b.cy - 12, (int)b.cx + 12, (int)b.cy + 12, [&](float x, float y) { float d = sdCircle(x, y, b.cx, b.cy, 9.5f); return fabsf(d) - 1.6f; }, rc);
	}
	for (auto &b : btn) {
		c.flat((int)b.cx - 9, (int)b.cy - 9, (int)b.cx + 9, (int)b.cy + 9, [&](float x, float y) { return sdCircle(x, y, b.cx, b.cy, 7.5f); }, C(60, 62, 74));
		c.fill((int)b.cx - 8, (int)b.cy - 8, (int)b.cx + 8, (int)b.cy + 8, [&](float x, float y) { return sdCircle(x, y, b.cx, b.cy, 6.5f); },
			[&](float x, float y, float) { float l = 1.15f - 0.35f * ((x - b.cx + 2) * (x - b.cx + 2) + (y - b.cy + 2) * (y - b.cy + 2)) / 42; if (l > 1.2f) l = 1.2f;
				Col r = { b.col.r * l, b.col.g * l, b.col.b * l }; if (r.r > 255) r.r = 255; if (r.g > 255) r.g = 255; if (r.b > 255) r.b = 255; return r; });
		glyph(c, b.g, (int)b.cx, (int)b.cy, C(255, 255, 255));
	}

	/* lettres L / R sur la partie visible des gâchettes */
	glyph(c, 4, trgX[0], 24, C(50, 52, 66));
	glyph(c, 5, trgX[1], 24, C(50, 52, 66));

	/* légende : carré de couleur devant chaque fonction */
	for (int f = 0; f < nFn && f < 4 && !s_noBg; f++) {
		uint16_t v = fnColour(f);
		for (int y = kLegendY + f * 8 + 1; y < kLegendY + f * 8 + 7; y++) for (int x = 8; x < 18; x++) px[y * pitchPx + x] = v;
	}
}


void drawSmall(uint16_t *dst, int dstPitchPx, int dstW, int dstH, uint32_t litBit) {
	static uint16_t big[W * H];
	const uint32_t assign[1] = { litBit };
	s_noBg = true;
	draw(big, W, assign, litBit ? 1 : 0, 0, 1);   /* fonction 0 choisie, clignotement allumé : anneau blanc */
	s_noBg = false;
	/* réduction de la manette (x 48..272, y 8..128) par moyenne des pixels opaques */
	const int sx0 = 48, sy0 = 8, sw = 224, sh = 120;
	for (int y = 0; y < dstH; y++)
		for (int x = 0; x < dstW; x++) {
			const int ax = sx0 + x * sw / dstW, bx = sx0 + (x + 1) * sw / dstW, ay = sy0 + y * sh / dstH, by = sy0 + (y + 1) * sh / dstH;
			int r = 0, g = 0, b = 0, n = 0, tot = 0;
			for (int yy = ay; yy < by; yy++) for (int xx = ax; xx < bx; xx++) {
				const uint16_t v = big[yy * W + xx]; tot++;
				if (!(v & 0x8000)) continue;
				r += (v >> 10) & 31; g += (v >> 5) & 31; b += v & 31; n++;
			}
			dst[y * dstPitchPx + x] = (n * 2 > tot) ? (uint16_t)(0x8000 | ((r / n) << 10) | ((g / n) << 5) | (b / n)) : 0;
		}
	/* anneau jaune bien visible autour du bouton à presser */
	struct { uint32_t bit; float cx, cy, rx, ry; } pos[6] = {
		{ InputMap::BTN_A, 216, 84, 13, 13 }, { InputMap::BTN_B, 231, 69, 13, 13 }, { InputMap::BTN_X, 201, 69, 13, 13 },
		{ InputMap::BTN_Y, 216, 54, 13, 13 }, { InputMap::BTN_L, 108, 30, 36, 19 }, { InputMap::BTN_R, 212, 30, 36, 19 } };
	for (auto &q : pos) {
		if (q.bit != litBit) continue;
		const float kx = (float)dstW / sw, ky = (float)dstH / sh;
		const float cx = (q.cx - sx0) * kx, cy = (q.cy - sy0) * ky, rx = q.rx * kx, ry = q.ry * ky;
		for (int y = 0; y < dstH; y++) for (int x = 0; x < dstW; x++) {
			const float ex = (x + 0.5f - cx) / rx, ey = (y + 0.5f - cy) / ry;
			const float d = sqrtf(ex * ex + ey * ey);
			if (d > 0.85f && d < 1.2f) dst[y * dstPitchPx + x] = rgb(255, 230, 40);
		}
	}
}


/* Style de la manette PlayStation d'origine du concours de danse : simple contour (gris au repos, orangé pendant un pas),
   intérieur transparent sur le rideau bleu, légèrement penchée vers la gauche, et une boule bleue lumineuse sur le
   bouton à presser. */
void drawDance(uint16_t *dst, int dstPitchPx, int dstW, int dstH, uint32_t litBit) {
	const float sx0 = 48, sy0 = 8, sw = 224, sh = 120;
	const float sc = (float)dstW / sw;                 /* pixels d'écran par unité du dessin */
	const float pcx = sx0 + sw / 2, pcy = sy0 + sh / 2;
	const float ang = -7.0f * 3.14159265f / 180, ca = cosf(ang), sa = sinf(ang);
	const float lw = 0.75f / sc;                       /* demi-épaisseur du trait (≈ 1,5 pixel) */
	const Col main = litBit ? C(255, 168, 96) : C(120, 132, 120), inner = litBit ? C(210, 120, 70) : C(84, 92, 84);
	struct { uint32_t bit; float cx, cy; } pos[6] = {
		{ InputMap::BTN_A, 216, 84 }, { InputMap::BTN_B, 231, 69 }, { InputMap::BTN_X, 201, 69 },
		{ InputMap::BTN_Y, 216, 54 }, { InputMap::BTN_L, 108, 30 }, { InputMap::BTN_R, 212, 30 } };
	float ox = -1000, oy = -1000;
	for (auto &q : pos) if (q.bit == litBit) { ox = q.cx; oy = q.cy; }
	for (int y = 0; y < dstH; y++)
		for (int x = 0; x < dstW; x++) {
			const float dx = (x + 0.5f - dstW / 2.0f) / sc, dy = (y + 0.5f - dstH / 2.0f) / sc;
			const float X = pcx + dx * ca + dy * sa, Y = pcy - dx * sa + dy * ca;
			uint16_t out = 0;
			/* contour extérieur : corps et gâchettes */
			float body = sdBody(X, Y);
			float tl = sdRBox(X, Y, 108, 30, 26, 12, 9), tr = sdRBox(X, Y, 212, 30, 26, 12, 9);
			float trig = tl < tr ? tl : tr;
			if (fabsf(body) < lw || (fabsf(trig) < lw && body > 0)) out = rgb((int)main.r, (int)main.g, (int)main.b);
			else {
				/* détails : VMU, stick, croix, Start, boutons */
				float d = fabsf(sdRBox(X, Y, 160, 59, 21, 19, 5));
				d = fminf(d, fabsf(sdCircle(X, Y, 102, 60, 12.5f)));
				float c1 = sdRBox(X, Y, 116, 96, 12, 4.5f, 1.5f), c2 = sdRBox(X, Y, 116, 96, 4.5f, 12, 1.5f);
				d = fminf(d, fabsf(c1 < c2 ? c1 : c2));
				d = fminf(d, fabsf(sdRBox(X, Y, 160, 99, 7, 4, 3.5f)));
				for (int k = 0; k < 4; k++) d = fminf(d, fabsf(sdCircle(X, Y, pos[k].cx, pos[k].cy, 6.5f)));
				if (d < lw * 0.8f && body < 0) out = rgb((int)inner.r, (int)inner.g, (int)inner.b);
			}
			/* boule bleue lumineuse sur le bouton à presser */
			const float r = sqrtf((X - ox) * (X - ox) + (Y - oy) * (Y - oy));
			if (r < 15) {
				const float t = r / 15;
				Col c;
				if (fabsf(r - 6) < 1.6f) c = C(250, 250, 255);                       /* anneau blanc */
				else if (t < 0.3f) c = C(150, 200, 255);
				else c = C((int)(40 + 60 * (1 - t)), (int)(80 + 90 * (1 - t)), (int)(190 + 50 * (1 - t)));
				out = rgb((int)c.r, (int)c.g, (int)c.b);
			}
			dst[y * dstPitchPx + x] = out;
		}
}

}
