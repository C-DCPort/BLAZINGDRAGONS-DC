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
#ifndef DRAGONS_SCREEN_H
#define DRAGONS_SCREEN_H

#include "graphics/surface.h"
#include "graphics/pixelformat.h"
#include "common/scummsys.h"
#include "common/rect.h"

namespace Dragons {
extern bool g_bdLangFr;
extern bool g_bdLangBoth;          /* disque avec les deux versions (dossier US) */
extern bool g_bdPerfHud;
extern bool g_bdPadPositional;       /* concours de danse : boutons de la manette à la place des boutons PlayStation */            /* jauges de temps par image affichées (option) */
extern int g_bdLangWanted;          /* langue choisie : 0 français, 1 anglais (réglages) */
extern void (*g_bdRestart)();       /* redémarrage du programme (changement de langue) */   /* disque France (sinon USA) : les fichiers HD propres à la version France ne servent qu'avec lui */
#define DRAGONS_NUM_PALETTES 5
#define DRAGONS_ENGINE_SPRITE_100_PERCENT_SCALE 256

#define DRAGONS_SCREEN_WIDTH 320
#define DRAGONS_SCREEN_HEIGHT 200

#define DRAGONS_NUM_FLAT_QUADS 0xf

#ifdef SCUMM_BIG_ENDIAN
	#define WRITE_SCREEN WRITE_BE_UINT16
	#define READ_SCREEN READ_BE_INT16
#else
	#define WRITE_SCREEN WRITE_LE_UINT16
	#define READ_SCREEN READ_LE_INT16
#endif

enum AlphaBlendMode {
	NONE,
	NORMAL,       // 50% x Back + 50% x Sprite
	ADDITIVE,     // 100% x Back + 100% x Sprite
	ADDITIVE_50,  // 100% x Back + 50% x Sprite
	SUBTRACTIVE   // 100% x Back - 100% x Sprite
};

struct FlatQuad {
	uint16 flags;
	uint16 priorityLayer;
	Common::Point points[4];
	uint16 colour;

	FlatQuad() {
		flags = 0;
		priorityLayer = 0;
		colour = 0;
	}
};

/**
 * Sortie graphique alternative (PVR de la Dreamcast). Si g_renderBackend n'est pas nul, Screen lui
 * transmet chaque primitive de dessin au lieu de composer l'image sur le CPU.
 */
class RenderBackend {
public:
	virtual ~RenderBackend() {}
	virtual void clear() = 0;
	/** Couche de décor 8 bits (palette du décor), répétée horizontalement. version : change si les pixels changent. */
	virtual void layer(const Graphics::Surface &s, uint32 version, const byte *palette, const Common::Rect &srcRect, AlphaBlendMode alpha) = 0;
	virtual void layerWrappedY(const Graphics::Surface &s, const byte *palette, int yOffset) = 0;
	virtual void sprite(const Graphics::Surface &s, const byte *palette, int x, int y, const Common::Rect &srcRect, bool flipX, AlphaBlendMode alpha, uint16 scale) = 0;
	virtual void bitmap16(const Graphics::Surface &s, int x, int y, const Common::Rect &srcRect, bool flipX, AlphaBlendMode alpha) = 0;
	virtual void fillRect(uint16 colour, const Common::Rect &r) = 0;
	/* image de vidéo en RGB555 (pitch en pixels) : texture opaque sans recherche de doublon */
	virtual void video16(const uint16 *px, int pitchPx, int w, int h, int x, int y, const Common::Rect &srcRect) { (void)px; (void)pitchPx; (void)w; (void)h; (void)x; (void)y; (void)srcRect; }
	/* Décor HD (textures VQ de HD.DAT). scene : nom du décor sur 4 caractères, layerNum : 0 fond, 1 milieu, 2 avant-plan.
	 * srcRect : fenêtre dans la couche, en pixels d'origine (comme layer()). Renvoie faux si cette couche n'existe pas
	 * en HD ou ne peut pas être chargée : le moteur dessine alors l'original. */
	/* Limite l'affichage des couches (décor HD ou original) à un rectangle de l'écran, tant que actif ; sans effet si non géré. */
	virtual void setLayerClip(bool on, int x0, int y0, int x1, int y1) { (void)on; (void)x0; (void)y0; (void)x1; (void)y1; }
	virtual bool hdLayer(const char *scene, int layerNum, const Common::Rect &srcRect, AlphaBlendMode alpha) { (void)scene; (void)layerNum; (void)srcRect; (void)alpha; return false; }
	/* Morceau de décor HD (image posée par un script) : dessiné entier, coin en (x, y) pixels d'écran d'origine, mélange alpha.
	 * draw faux : vérifie seulement qu'il existe et se charge. */
	virtual bool hdPatch(const char *name, int layerNum, int x, int y, bool draw) { (void)name; (void)layerNum; (void)x; (void)y; (void)draw; return false; }
	/* Sprite HD (HDSPR.DAT) : fichier d'acteur, décalage de l'en-tête de trame, genre de palette (0 propre, 1 commune, 2 du décor), position et taille d'origine de la trame (w, h pixels),
	 * échelle 0x100 = 100 %. Renvoie faux si cette trame n'existe pas en HD : le moteur dessine alors l'original. */
	virtual bool hdSprite(int actorFile, uint16 frameOffset, int paletteKind, int x, int y, bool flipX, AlphaBlendMode alpha, uint16 scale) { (void)actorFile; (void)frameOffset; (void)paletteKind; (void)x; (void)y; (void)flipX; (void)alpha; (void)scale; return false; }
	virtual void present(int shakeX, int shakeY) = 0;
};
extern uint32 g_hdLayTry, g_hdLayNotPristine, g_hdLayPal, g_hdLayHit, g_hdLayFail;   /* diagnostic décors */
extern char g_hdLayFailName[16];
struct HdLayStat { char name[5]; int layer; unsigned hit, fail, pal, notp; int mode; };
extern HdLayStat g_hdLayStat[32]; extern int g_hdLayStatN;
void hdLayStatNote(const char *scene, int layer, bool hit, int mode, int kind = 0);
extern int g_hdPalDiff[4];
extern int g_hdPalDump[16];
extern int g_hdWhite[8][6], g_hdWhiteN;
extern uint32 g_hdSprPal0, g_hdSprPal4;   /* parmi ces sprites : palette 0 (décor) / palette 4 */
extern uint32 g_hdSprPalMiss;         /* sprites non dessinés en HD parce qu'ils n'utilisent pas leur palette propre (statistiques) */
extern bool g_fullScreen;           /* option Affichage : vrai = image étirée sur les 480 lignes (sans bandes noires) */
extern bool g_spriteExactUV;        /* texte HD : sprite réduit dessiné avec les coordonnées de texture exactes */
extern byte g_hdPal1Ref[512];      /* palette 1 de référence des sprites HD du mini-jeu des pouces (au lieu de celle du curseur) */
extern bool g_hdPal1RefOn;
extern int g_spriteNoSoft;          /* > 0 : texte et cadres, mini-jeu des lapins : jamais de flou des sprites demi-transparents */
extern bool g_hdGraphics;            /* option Graphismes : vrai = décors HD quand ils existent */
extern RenderBackend *g_renderBackend;
extern bool g_renderMirror;   /* vrai : dessiner aussi en logiciel (tests de comparaison) */

class Screen {
private:
	Graphics::PixelFormat _pixelFormat;
	Graphics::Surface *_backSurface;
	byte _palettes[DRAGONS_NUM_PALETTES][512];
	Common::Point _screenShakeOffset;
	FlatQuad _flatQuads[DRAGONS_NUM_FLAT_QUADS];
public:
	virtual ~Screen();

	Screen();

	Graphics::PixelFormat getPixelFormat() { return _pixelFormat; }
	void copyVideoFrame(const Graphics::Surface &srcSurface, int destX, int destY, const Common::Rect &srcRect);
	void copyRectToSurface(const Graphics::Surface &srcSurface, int destX, int destY);
	void copyRectToSurface(const Graphics::Surface &srcSurface, int destX, int destY, const Common::Rect &srcRect, bool flipX = false, AlphaBlendMode alpha = NONE);
	void copyRectToSurface8bpp(const Graphics::Surface &srcSurface, const byte *palette, int destX, int destY, const Common::Rect &srcRect, bool flipX = false, AlphaBlendMode alpha = NONE, uint16 scale = DRAGONS_ENGINE_SPRITE_100_PERCENT_SCALE);
	void copyRectToSurface8bppWrappedX(const Graphics::Surface &srcSurface, const byte *palette, const Common::Rect &srcRect, AlphaBlendMode alpha = NONE, uint32 version = 0);
	void updateScreen();
	void loadPalette(uint16 paletteNum, const byte *palette);
	byte *getPalette(uint16 paletteNum);
	void setPaletteRecord(uint16 paletteNum, uint16 offset, uint16 newValue);
	void updatePaletteTransparency(uint16 paletteNum, uint16 startOffset, uint16 endOffset, bool isTransparent);
	void clearScreen();
	void drawRect(uint16 colour, Common::Rect rect, int id);
	void fillRect(uint16 colour, Common::Rect rect);
	Common::Rect clipRectToScreen(int destX, int destY, const Common::Rect &rect);
	Common::Rect clipRectToRect(int destX, int destY, const Common::Rect &rect, const Common::Rect &containerRect);

	void setScreenShakeOffset(int16 x, int16 y);

	void copyRectToSurface8bppWrappedY(const Graphics::Surface &srcSurface, const byte *palette, int yOffset);

	int16 addFlatQuad(int16 x0, int16 y0, int16 x1, int16 y1, int16 x3, int16 y3, int16 x2, int16 y2, uint16 colour, int16 priorityLayer, uint16 flags);
	void drawFlatQuads(uint16 priorityLayer);
	FlatQuad *getFlatQuad(uint16 quadId);
	void clearAllFlatQuads();

private:
	void copyRectToSurface(const void *buffer, int srcPitch, int srcWidth, int srcXOffset, int destX, int destY, int width, int height, bool flipX, AlphaBlendMode alpha);
	void copyRectToSurface8bpp(const void *buffer, const byte* palette, int srcPitch, int srcWidth, int srcXOffset, int destX, int destY, int width, int height, bool flipX, AlphaBlendMode alpha);
	void drawScaledSprite(Graphics::Surface *destSurface, const byte *source, int sourceWidth, int sourceHeight, int destX, int destY, int destWidth, int destHeight, const byte *palette, bool flipX, AlphaBlendMode alpha);
};

} // End of namespace Dragons

#endif //DRAGONS_SCREEN_H
