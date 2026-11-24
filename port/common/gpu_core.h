/* Noyau commun de la composition par la puce graphique (PC pour les tests, PVR sur Dreamcast).
 *
 * Core reçoit les primitives de dessin du moteur (interface Dragons::RenderBackend), fabrique des
 * textures 16 bits à partir des surfaces 8 bits + palette, les met en cache, découpe les grands décors
 * en tuiles, et fournit à un Sink une liste de quads dans l'ordre de dessin (painter's algorithm).
 *
 * Coordonnées : pixels du jeu (fenêtre 320×200). Le Sink les met à l'échelle de l'écran.
 */
#ifndef BD_GPU_CORE_H
#define BD_GPU_CORE_H

#include "dragons/screen.h"
#include "hd_pack.h"
#include <vector>
#include <map>
#include <set>
#include <string.h>

namespace Gpu {

enum { FMT_1555 = 0, FMT_4444 = 1 };
enum { BL_ALPHA = 0, BL_ADD = 1 };

/* Côté matériel : PVR sur Dreamcast, rastériseur logiciel dans le simulateur. */
class Sink {
public:
	virtual ~Sink() {}
	/* w et h : puissances de 2. pixels : ARGB1555 ou ARGB4444, w*h valeurs. Renvoie 0 en cas d'échec. */
	virtual uint32 texCreate(int w, int h, int fmt, const uint16 *pixels) = 0;
	/* Texture VQ twiddlée size×size : 2048 octets de codebook puis (size/2)² indices. fmt : 0 RGB565, 1 ARGB1555, 2 ARGB4444.
	 * Renvoie 0 si le Sink ne sait pas (le décor HD est alors ignoré) ou en cas d'échec. */
	virtual uint32 texCreateVQ(int size, int fmt, const uint8_t *data) { (void)size; (void)fmt; (void)data; return 0; }
	virtual void texDestroy(uint32 handle) = 0;
	virtual void beginFrame() = 0;
	/* Liste du PVR pour les primitives suivantes : opaque (rendu bien moins coûteux) ou translucide (ordre d'envoi). */
	virtual void opaqueList(bool on) { (void)on; }
	virtual void quad(uint32 tex, float x0, float y0, float x1, float y1,
	                  float u0, float v0, float u1, float v1, int blend, int alpha) = 0;
	virtual void solid(float x0, float y0, float x1, float y1, uint16 rgb555) = 0;
	/* Textures à palette (8 bits par texel) : les couches du décor ne sont plus reconstruites quand seule la palette change
	   (eau animée…). 4 palettes de 256 couleurs. pal : couleurs du moteur (0 transparent ; mode 1 : bit 15 = demi-transparent). */
	virtual bool hasPal8() const { return false; }
	virtual uint32 texCreatePal8(int size, const uint8_t *idx) { (void)size; (void)idx; return 0; }   /* size×size, ligne par ligne */
	virtual void setPalette(int bank, const uint16 *pal, int mode) { (void)bank; (void)pal; (void)mode; }
	virtual void quadPal(uint32 tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, int blend, int bank) {
		(void)bank; quad(tex, x0, y0, x1, y1, u0, v0, u1, v1, blend, 255); }
	virtual void endFrame() = 0;
};

struct Stats {
	uint32 hdSprHit = 0, hdSprMissNoFrame = 0, hdSprMissOther = 0;
		uint32 missByActor[1024] = {0}; uint8 missKinds[1024] = {0};   /* diagnostic : trames absentes par fichier d'acteur */
	uint32 texCreated = 0, texFailed = 0, texEvicted = 0, quads = 0, skipped = 0, hdLoadRetry = 0;
	size_t bytesCreated = 0;
	double area = 0, areaOpq = 0;   /* surface dessinée (pixels du jeu), cumul : mesure de la charge du PVR */
};

class Core : public Dragons::RenderBackend {
public:
	Core(Sink *sink, size_t budgetBytes);
	~Core();

	void clear() override;
	void layer(const Graphics::Surface &s, uint32 version, const byte *palette, const Common::Rect &srcRect, Dragons::AlphaBlendMode alpha) override;
	void layerWrappedY(const Graphics::Surface &s, const byte *palette, int yOffset) override;
	void sprite(const Graphics::Surface &s, const byte *palette, int x, int y, const Common::Rect &srcRect, bool flipX, Dragons::AlphaBlendMode alpha, uint16 scale) override;
	void bitmap16(const Graphics::Surface &s, int x, int y, const Common::Rect &srcRect, bool flipX, Dragons::AlphaBlendMode alpha) override;
	void fillRect(uint16 colour, const Common::Rect &r) override;
	void video16(const uint16 *px, int pitchPx, int w, int h, int x, int y, const Common::Rect &srcRect) override;
	void setLayerClip(bool on, int x0, int y0, int x1, int y1) override;
	bool hdLayer(const char *scene, int layerNum, const Common::Rect &srcRect, Dragons::AlphaBlendMode alpha) override;
	bool hdPatch(const char *name, int layerNum, int x, int y, bool draw) override;
	bool hdSprite(int actorFile, uint16 frameOffset, int paletteKind, int x, int y, bool flipX, Dragons::AlphaBlendMode alpha, uint16 scale) override;
	void present(int shakeX, int shakeY) override;
	void setHdPack(HdPack *p) { _hd = p; }
	void setHdSprPack(HdSprPack *p) { _hdSpr = p; }

	Stats stats;
	size_t usedBytes() const { return _used; }
	size_t opCount() const { return _ops.size(); }

private:
	struct Key {
		uint32 k[6];
		bool operator<(const Key &o) const { return memcmp(k, o.k, sizeof(k)) < 0; }
	};
	/* bx0..by1 : rectangle utile de la texture (fractions 0..1) ; les parties entièrement transparentes ne sont pas
	   envoyées au PVR (couches mg/fg : surtout du vide, chaque pixel translucide coûte du temps de rendu). Vide : bx0 >= bx1. */
	struct Tex { uint32 handle; size_t bytes; uint32 lastUse; float bx0 = 0, by0 = 0, bx1 = 1, by1 = 1; bool opq = false;
		float iu0 = 0, iv0 = 0, iu1 = 0, iv1 = 0;   /* plus grand rectangle entièrement opaque (fractions) : cache ce qui est dessous */ };
	void vqBounds(const byte *buf, int fmt, Tex &t);
	struct LayerEntry { int w, h; uint32 lastUse; std::map<int, Tex> tiles; bool soft = false;
		std::map<int, std::vector<uint32>> cells;   /* textures à palette : par tuile, indices présents dans chaque case 32×32 (64 × 8 mots) */
		std::map<int, std::vector<uint16>> ibox;   /* textures à palette : par tuile, rectangle de chaque indice (4 × 256) */ };
	/* banques de palette : contenu (empreinte), dernière image d'utilisation */
	uint32 _bankHash[4] = {0, 0, 0, 0}; uint32 _bankUse[4] = {0, 0, 0, 0}; bool _bankSet[4] = {false, false, false, false};
	int _bankNext = -1;
	bool _fullNext = false;
	void promoteOpaque();
	int paletteBank(const uint16 *pal, int mode);
	bool layerPal(const Graphics::Surface &s, uint32 version, const uint16 *pal, int mode, int left, int top, int winW, int winH);
	struct Op {
		uint32 tex; float x0, y0, x1, y1, u0, v0, u1, v1; int blend, alpha; uint16 col; bool solid; bool opq = false;   /* liste opaque du PVR (première couche de l'image) */
		bool full = false;   /* texture sans aucun texel transparent ni demi-transparent */
		float ox0 = 0, oy0 = 0, ox1 = 0, oy1 = 0;   /* partie entièrement opaque à l'écran (vide : ox0 >= ox1) */
		int bank = -1;   /* texture à palette : banque de palette */
	};
	struct Pending { uint32 handle; size_t bytes; uint32 frame; };

	Sink *_sink;
	size_t _budget, _used;
	uint32 _frame;
	uint32 _videoSeq;
	uint16 *_stage;                 /* tampon de conversion (512×256 pixels) */
	std::vector<Op> _ops;
	std::map<Key, Tex> _tex;        /* sprites et bitmaps */
	std::map<Key, LayerEntry> _layers;
	struct HdLayerEntry { LayerEntry l; const HdEntry *e; bool failed; uint32 retryAt; int tries; };
	bool _occNext = false; float _occ[4] = {0, 0, 0, 0};   /* rectangle opaque (fractions de texture) du prochain addQuad */
	void occludeOps();
	std::vector<Op> _occOut;
	bool _opqLayer = false, _opqNext = false;   /* couche dessinée en premier : ses tuiles sans demi-transparence vont dans la liste opaque */
	bool _clipOn = false; float _cx0 = 0, _cy0 = 0, _cx1 = 0, _cy1 = 0;   /* rectangle d'écran des couches (setLayerClip) */
	std::map<uint32, HdLayerEntry> _hdLayers;   /* clé : (4 octets du nom de scène) ^ (couche << 28) */
	HdPack *_hd = nullptr;
	HdSprPack *_hdSpr = nullptr;
	std::map<uint32, Tex> _hdPages;       /* pages de sprites HD chargées (clé : numéro de page) */
	std::set<uint32> _hdPagesFailed;
	bool hdLoad(HdLayerEntry &h);
	std::vector<Pending> _pending;  /* textures à libérer dans 2 images (le GPU peut encore les lire) */

	bool makeRoom(size_t need);
	uint32 createTex(int w, int h, int fmt, size_t &bytes);
	void releaseTex(Tex &t);
	void dropOpsUsing(uint32 handle);
	void addQuad(uint32 tex, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, int blend);
	template <class F> bool build(int w, int h, int mode, F get, int &tw, int &th, int &fmt);
	void drawTiles(LayerEntry &e, const Graphics::Surface &s, const uint16 *pal, int mode, int blend,
	               int left, int top, int winW, int winH, bool wrapY);
	Tex *layerTile(LayerEntry &e, int tx, int ty, const Graphics::Surface &s, const uint16 *pal, int mode, int &fmtOut);
};

} // namespace Gpu

#endif
