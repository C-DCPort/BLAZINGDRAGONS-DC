/* Lecteur de HD.DAT : décors HD en textures VQ de 256x256 (voir tools/hd_pack.py pour le format). */
#ifndef BD_HD_PACK_H
#define BD_HD_PACK_H

#include "common/scummsys.h"
#include <vector>

namespace Disc { struct File; }   /* lecture par gros blocs de secteurs (disc.h) : fs_iso9660 lit 130 Ko/s sinon */

namespace Gpu {

enum { HD_TILE = 256, HD_TILE_BYTES = 2048 + (HD_TILE / 2) * (HD_TILE / 2) };
enum { HD_FMT_565 = 0, HD_FMT_1555 = 1, HD_FMT_4444 = 2 };   /* codes du fichier (pas ceux de Gpu::FMT_*) */

struct HdEntry {
	char scene[9];
	uint8 layer, fmt;       /* fmt : 0 RGB565, 1 ARGB1555, 2 ARGB4444 */
	uint8 tilesX, tilesY;
	uint16 w, h;            /* pixels HD (2 par pixel d'origine en largeur) */
	uint16 oh;              /* hauteur d'origine (pixels du jeu) : h/2, ou 200 pour une couche 480 lignes */
	uint32 offset, size;
};

class HdPack {
public:
	HdPack();
	~HdPack();
	/* Cherche HD.DAT dans le dossier des données. Renvoie faux s'il est absent ou invalide. */
	bool open(const char *name = "HD.DAT");
	bool isOpen() const { return _f != nullptr; }
	int count() const { return (int)_entries.size(); }
	const HdEntry *find(const char *scene, int layer) const;
	/* Lit toutes les tuiles de la couche d'un seul bloc (dst : size octets, de préférence aligné 32). */
	bool read(const HdEntry &e, void *dst);
	bool readTile(const HdEntry &e, int i, void *dst);   /* une tuile (HD_TILE_BYTES) de la couche */
private:
	Disc::File *_f;
	std::vector<HdEntry> _entries;
};

/* Sprites HD (HDSPR.DAT, voir tools/spr_pack.py) : trames d'acteurs en morceaux dans des pages VQ de 256x256. */
struct HdSprPiece { uint32 page; uint16 x, y, w, h, dx, dy; uint8 fmt; };   /* fmt : format VQ de la page (0 565, 1 1555, 2 4444) */   /* contenu dans la page / position dans la trame HD */
struct HdSprFrame { uint16 keyLo; uint16 firstPiece; uint8 nPieces; uint16 w, h; const HdSprPiece *pieces; };   /* w, h : pixels d'origine */
/* Index chargé à la demande, par groupe (genre + fichier d'acteur) : avec tous les personnages du jeu, l'index complet
   prendrait plus de 350 Ko de RAM. Seuls les groupes récemment utilisés restent en mémoire (budget HD_SPR_INDEX_BUDGET). */
class HdSprPack {
public:
	HdSprPack();
	~HdSprPack();
	bool open(const char *name = "HDSPR.DAT");
	bool isOpen() const { return _f != nullptr; }
	int frameCount() const { return (int)_nFrames; }
	/* kind : 0 palette propre de l'acteur, 1 palette commune du moteur (palette 1), 2 palette du décor (palette 0).
	   Le pointeur reste valable jusqu'au prochain appel de find(). */
	const HdSprFrame *find(int actorFile, uint16 frameOffset, int kind);
	const HdSprPiece &piece(const HdSprFrame *f, int i) const { return f->pieces[f->firstPiece + i]; }
	int pageSize() const { return _pageSize; }        /* côté des pages VQ (256 en v2, 64 en v3) */
	uint32 pageBytes() const { return _pageBytes; }
	/* Lit une page (HD_TILE_BYTES octets). */
	bool readPage(uint32 page, void *dst);
	/* Lit n pages consécutives en une seule lecture (un seul déplacement de la tête, lent sur GD-ROM). */
	bool readPages(uint32 page, int n, void *dst);
	uint32 pageCount() const { return _nPages; }
	uint32 indexBytes() const { return _cacheBytes; }   /* index en mémoire (statistiques) */
	uint32 indexLoads() const { return _loads; }
private:
	struct Group { uint16 id, nFrames, nPieces; uint32 off, size; };
	struct Cached { uint16 id; uint32 lastUse; std::vector<HdSprFrame> frames; std::vector<HdSprPiece> pieces; uint32 bytes; };
	Cached *loadGroup(const Group &g);
	Disc::File *_f;
	int _pageSize = 256; uint32 _pageBytes = 18432, _nPages = 0, _version = 2;
	uint32 _nFrames = 0, _dataStart = 0, _use = 0, _cacheBytes = 0, _loads = 0;
	std::vector<Group> _groups;
	std::vector<uint8> _pageFmt;
	std::vector<Cached *> _cache;
	Group _missGroup = { 0xffff, 0, 0, 0, 0 };
};

} // namespace Gpu
#endif
