/* Sortie PVR de la composition par la puce graphique (Dreamcast, KOS).
 *
 * Tout passe dans la liste translucide, autosort désactivé : les polygones sont rendus dans l'ordre d'envoi,
 * ce qui reproduit l'ordre de dessin du moteur (décor, sprites, rectangles). Test de profondeur toujours vrai,
 * pas d'écriture Z. Coordonnées du jeu (320×200) agrandies ×2 et centrées dans 640×480 (bandes noires 40 px).
 */
#ifndef BD_GPU_SINK_DC_H
#define BD_GPU_SINK_DC_H
#include "bdprof.h"
#include <kos.h>
#include <map>
#include "gpu_core.h"

namespace Dragons { extern bool g_fullScreen; }
class DcPvrSink : public Gpu::Sink {
	struct T { pvr_ptr_t ptr; int w, h, fmt; bool vq; bool pal = false; };   /* vq : texture VQ twiddlée (HD), fmt = code du fichier HD ; pal : 8 bits à palette */
	bool _palFmtSet = false;
	int _lastBank = -1;
	std::map<uint32, T> _tex;
	uint32 _next = 1;
	uint32 _lastTex = 0;
	int _lastBlend = -1;
	pvr_list_t _list = PVR_LIST_TR_POLY; bool _open = false;   /* liste PVR ouverte (_open) : PVR_LIST_OP_POLY ou PVR_LIST_TR_POLY */

	void openList(pvr_list_t l) {
		if (_open && _list == l) return;
		if (_open) pvr_list_finish();
		pvr_list_begin(l);
		_list = l; _open = true; _lastTex = 0; _lastBlend = -1;
	}

	static inline float SX(float x) { return x * 2.0f; }
	static inline float SY(float y) { return Dragons::g_fullScreen ? y * 2.4f : y * 2.0f + 40.0f; }

	static void vertex(float x, float y, float u, float v, bool eol, uint32 argb) {
		pvr_vertex_t vt;
		vt.flags = eol ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
		vt.x = SX(x); vt.y = SY(y); vt.z = 1.0f;
		vt.u = u; vt.v = v;
		vt.argb = argb; vt.oargb = 0;
		pvr_prim(&vt, sizeof(vt));
	}

public:
	/* Mesures à l'écran (build « perf ») : rectangles pleins ajoutés par main_dc, dessinés par-dessus l'image */
	struct HudRect { float x0, y0, x1, y1; uint16 c; };
	HudRect hud[24]; int nHud = 0;

	~DcPvrSink() {
		for (auto &t : _tex) pvr_mem_free(t.second.ptr);
	}

	size_t vramFree() const { return pvr_mem_available(); }
	size_t texCount() const { return _tex.size(); }

	uint32 texCreate(int w, int h, int fmt, const uint16 *pixels) override {
		const size_t bytes = (size_t)w * h * 2;
		pvr_ptr_t p = pvr_mem_malloc(bytes);
		if (!p) return 0;
		dcache_flush_range((uintptr_t)pixels, bytes);
		pvr_txr_load(pixels, p, bytes);
		T t = { p, w, h, fmt, false };
		_tex[_next] = t;
		return _next++;
	}

	/* Texture VQ twiddlée size×size (décors et sprites HD) : codebook 2048 octets puis (size/2)² indices.
	 * fmt : 0 RGB565, 1 ARGB1555, 2 ARGB4444. NON VÉRIFIÉ sur console : l'ordre des texels de hd_pack.py vient de la
	 * documentation du PVR, pas d'un essai matériel. */
	uint32 texCreateVQ(int size, int fmt, const uint8_t *data) override {
		if (fmt < 0 || fmt > 2 || size < 8 || (size & (size - 1))) return 0;
		const size_t bytes = 2048 + (size_t)(size / 2) * (size / 2);
		pvr_ptr_t p = pvr_mem_malloc(bytes);
		if (!p) return 0;
		dcache_flush_range((uintptr_t)data, bytes);
		pvr_txr_load(data, p, bytes);
		T t = { p, size, size, fmt, true };
		_tex[_next] = t;
		return _next++;
	}

	/* Textures à palette 8 bits (couches du décor) : twiddlées au chargement, palettes ARGB1555 (4 banques de 256). */
	bool hasPal8() const override { return true; }
	uint32 texCreatePal8(int size, const uint8_t *idx) override {
		const size_t bytes = (size_t)size * size;
		pvr_ptr_t p = pvr_mem_malloc(bytes);
		if (!p) return 0;
		pvr_txr_load_ex((void *)idx, p, size, size, PVR_TXRLOAD_8BPP);
		T t = { p, size, size, -1, false, true };
		_tex[_next] = t;
		return _next++;
	}
	void setPalette(int bank, const uint16 *pal, int mode) override {
		(void)mode;   /* pas de demi-transparence ici (gpu_core garde l'ancien chemin pour ces couches) */
		if (!_palFmtSet) { pvr_set_pal_format(PVR_PAL_ARGB1555); _palFmtSet = true; }   /* 8888 : rendu deux fois plus lent */
		for (int i = 0; i < 256; i++) {
			const uint16 c = pal[i];
			pvr_set_pal_entry(bank * 256 + i, c ? (uint32)(0x8000 | (c & 0x7fff)) : 0);
		}
	}
	void quadPal(uint32 tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, int blend, int bank) override {
		auto it = _tex.find(tex);
		if (it == _tex.end()) return;
		if (!_open) openList(PVR_LIST_TR_POLY);
		if (tex != _lastTex || blend != _lastBlend || bank != _lastBank) {
			const T &t = it->second;
			pvr_poly_cxt_t cxt;
			pvr_poly_cxt_txr(&cxt, _list, PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(bank) | PVR_TXRFMT_TWIDDLED,
				t.w, t.h, t.ptr, PVR_FILTER_NEAREST);
			cxt.gen.culling = PVR_CULLING_NONE;
			cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
			cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
			if (blend == Gpu::BL_ADD) { cxt.blend.src = PVR_BLEND_ONE; cxt.blend.dst = PVR_BLEND_ONE; }
			pvr_poly_hdr_t hdr;
			pvr_poly_compile(&hdr, &cxt);
			pvr_prim(&hdr, sizeof(hdr));
			_lastTex = tex; _lastBlend = blend; _lastBank = bank;
		}
		vertex(x0, y1, u0, v1, false, 0xffffffff);
		vertex(x0, y0, u0, v0, false, 0xffffffff);
		vertex(x1, y1, u1, v1, false, 0xffffffff);
		vertex(x1, y0, u1, v0, true, 0xffffffff);
	}

	void texDestroy(uint32 handle) override {
		auto it = _tex.find(handle);
		if (it == _tex.end()) return;
		pvr_mem_free(it->second.ptr);
		_tex.erase(it);
	}

	void beginFrame() override {
		{ BdProf::Scope prof(BdProf::WAIT); pvr_wait_ready(); }
		pvr_scene_begin();
		_open = false; _lastTex = 0; _lastBlend = -1;
	}

	/* Première couche du décor (sans transparence) dans la liste opaque : le PVR la rend bien plus vite que
	   des polygones translucides ; elle passe avant la liste translucide, donc toujours dessous, comme avant. */
	void opaqueList(bool on) override { openList(on ? PVR_LIST_OP_POLY : PVR_LIST_TR_POLY); }

	void quad(uint32 tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, int blend, int alpha) override {
		auto it = _tex.find(tex);
		if (it == _tex.end()) return;
		if (!_open) openList(PVR_LIST_TR_POLY);
		if (tex != _lastTex || blend != _lastBlend) {   /* même texture et même mélange : on garde l'en-tête */
			const T &t = it->second;
			pvr_poly_cxt_t cxt;
			if (t.vq) {
				static const int vqfmt[3] = { PVR_TXRFMT_RGB565, PVR_TXRFMT_ARGB1555, PVR_TXRFMT_ARGB4444 };
				pvr_poly_cxt_txr(&cxt, _list, vqfmt[t.fmt] | PVR_TXRFMT_VQ_ENABLE | PVR_TXRFMT_TWIDDLED,
					t.w, t.h, t.ptr, PVR_FILTER_BILINEAR);
			} else
			pvr_poly_cxt_txr(&cxt, _list,
				(t.fmt == Gpu::FMT_4444 ? PVR_TXRFMT_ARGB4444 : PVR_TXRFMT_ARGB1555) | PVR_TXRFMT_NONTWIDDLED,
				t.w, t.h, t.ptr, PVR_FILTER_NEAREST);
			cxt.gen.culling = PVR_CULLING_NONE;
			cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
			cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
			if (blend == Gpu::BL_ADD) { cxt.blend.src = PVR_BLEND_ONE; cxt.blend.dst = PVR_BLEND_ONE; }
			pvr_poly_hdr_t hdr;
			pvr_poly_compile(&hdr, &cxt);
			pvr_prim(&hdr, sizeof(hdr));
			_lastTex = tex; _lastBlend = blend; _lastBank = -1;
		}
		const uint32 argb = ((uint32)(alpha & 255) << 24) | 0x00ffffff;
		vertex(x0, y1, u0, v1, false, argb);
		vertex(x0, y0, u0, v0, false, argb);
		vertex(x1, y1, u1, v1, false, argb);
		vertex(x1, y0, u1, v0, true, argb);
	}

	void solid(float x0, float y0, float x1, float y1, uint16 rgb555) override {
		if (!_open) openList(PVR_LIST_TR_POLY);
		pvr_poly_cxt_t cxt;
		pvr_poly_cxt_col(&cxt, _list);
		cxt.gen.culling = PVR_CULLING_NONE;
		cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
		cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
		pvr_poly_hdr_t hdr;
		pvr_poly_compile(&hdr, &cxt);
		pvr_prim(&hdr, sizeof(hdr));
		_lastTex = 0; _lastBlend = -1;
		const uint32 r5 = (rgb555 >> 10) & 31, g5 = (rgb555 >> 5) & 31, b5 = rgb555 & 31;
		const uint32 argb = 0xff000000u | (((r5 << 3) | (r5 >> 2)) << 16) | (((g5 << 3) | (g5 >> 2)) << 8) | ((b5 << 3) | (b5 >> 2));
		vertex(x0, y1, 0, 0, false, argb);
		vertex(x0, y0, 0, 0, false, argb);
		vertex(x1, y1, 0, 0, false, argb);
		vertex(x1, y0, 0, 0, true, argb);
	}

	void endFrame() override {
		openList(PVR_LIST_TR_POLY);   /* les mesures (perf) sont toujours dans la liste translucide, par-dessus */
		for (int i = 0; i < nHud; i++) solid(hud[i].x0, hud[i].y0, hud[i].x1, hud[i].y1, hud[i].c);
		pvr_list_finish(); _open = false;
		pvr_scene_finish();
	}
};
#endif
