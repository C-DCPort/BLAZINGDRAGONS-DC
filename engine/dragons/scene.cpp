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
#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "dragons/scene.h"
#include "bdprof.h"
#include <map>
#include <vector>
#include <stdlib.h>
extern int g_hdSprWhy;
extern int g_hdSprExpectW, g_hdSprExpectH;
#include "dragons/dragons.h"
#include "dragons/scenehint.h"
#include "dragons/actor.h"
#include "dragons/background.h"
#include "dragons/cursor.h"
#include "dragons/credits.h"
#include "dragons/dragonini.h"
#include "dragons/dragonimg.h"
#include "dragons/font.h"
#include "dragons/inventory.h"
#include "dragons/screen.h"
#include "dragons/actorresource.h"
#include "dragons/scriptopcodes.h"

namespace Dragons {

/* Les images HD ont les couleurs de la palette d'origine : elles ne s'affichent que si les scripts ne l'ont pas modifiée
   (l'entrée 0 et le bit de transparence de l'entrée 1 sont retouchés au chargement, on compare le reste). */
/* Entrees de palette réellement utilisées par une couche (mémorisées par scène/couche/version) : une scène peut recolorer des entrées
   qui ne servent qu'aux sprites (dialogues, éclairages) sans toucher aux couleurs du décor. */
struct LayerUse { const void *stage; const void *surf; uint32 ver; bool valid; bool used[256]; };
static LayerUse g_layerUse[3];
static const bool *layerUsedColours(const void *stage, uint8 n, uint32 ver, const Graphics::Surface *surf) {
	if (n >= 3 || !surf || surf->format.bytesPerPixel != 1) return nullptr;
	LayerUse &u = g_layerUse[n];
	if (!u.valid || u.stage != stage || u.surf != surf || u.ver != ver) {
		for (int i = 0; i < 256; i++) u.used[i] = false;
		for (int y = 0; y < surf->h; y++) {
			const byte *row = (const byte *)surf->getBasePtr(0, y);
			for (int x = 0; x < surf->w; x++) u.used[row[x]] = true;
		}
		u.stage = stage; u.surf = surf; u.ver = ver; u.valid = true;
	}
	return u.used;
}
/* Quelques couleurs changées (animation de palette : eau, ligne du pêcheur, carte) ne font plus repasser tout le décor en
   original, ce qui le faisait clignoter : on tolère jusqu'à 1/16 des couleurs utilisées (au moins 3). Un fondu, qui
   change toutes les couleurs, reste refusé. */
static bool s_palAnim[256];   /* couleurs animées tolérées au dernier contrôle (redessinées par-dessus le décor HD) */
static int s_palAnimN = 0;
static bool s_bgPalOk = false;   /* palette du fond (couche 0) intacte à cette image */
static bool paletteIntactUsed(const byte *cur, const byte *ref, const bool *used) {
	int nUsed = 0, nDiff = 0;
	memset(s_palAnim, 0, sizeof(s_palAnim)); s_palAnimN = 0;
	for (int i = 2; i < 256; i++) {
		if (used && !used[i]) continue;
		nUsed++;
		const uint16 a = READ_LE_UINT16(cur + i * 2) & 0x7fff;
		const uint16 b = READ_LE_UINT16(ref + i * 2) & 0x7fff;
		if (a != b && (a > 1 || b > 1)) { if (!nDiff) { g_hdPalDiff[1] = i; g_hdPalDiff[2] = a; g_hdPalDiff[3] = b; } nDiff++; s_palAnim[i] = true; }
	}
	s_palAnimN = nDiff;
	return nDiff <= MAX(3, nUsed / 2);   /* fondu = presque toutes les couleurs ; animation (eau, arbre de s12a) : moins de la moitié */
}

/* sprite peint avec la palette du décor : seules les couleurs qu'il utilise comptent (l'eau animée du lac
   change d'autres entrées et faisait repasser le pêcheur en original quelques images) */
static bool spritePaletteOk(const byte *cur, const byte *ref, const Graphics::Surface *s) {
	if (!s || !s->getPixels() || s->format.bytesPerPixel != 1) return false;
	bool used[256] = { false };
	const byte *px = (const byte *)s->getPixels();
	for (int j = 0; j < s->h; j++) for (int i = 0; i < s->w; i++) used[px[j * s->pitch + i]] = true;
	int nUsed = 0, nDiff = 0;
	for (int i = 2; i < 256; i++) {
		if (!used[i]) continue;
		nUsed++;
		const uint16 a = READ_LE_UINT16(cur + i * 2) & 0x7fff, b = READ_LE_UINT16(ref + i * 2) & 0x7fff;
		if (a != b && (a > 1 || b > 1)) nDiff++;
	}
#ifdef BD_SIM_DIAG
	if (BdProf::envFlag("BD_SPRPAL") && nDiff > MAX(2, nUsed / 8)) {
		BdProf::say("SPRPAL used %d diff %d :", nUsed, nDiff);
		for (int i = 2; i < 256; i++) if (used[i]) { const uint16 a = READ_LE_UINT16(cur + i * 2) & 0x7fff, b = READ_LE_UINT16(ref + i * 2) & 0x7fff; if (a != b && (a > 1 || b > 1)) BdProf::say(" %d:%04x/%04x", i, a, b); }
		BdProf::say("\n");
	}
#endif
	return nDiff <= MAX(2, nUsed / 8);   /* fondu de scène = presque toutes les couleurs : original */
}

static bool paletteIntact(const byte *cur, const byte *ref, int bytes) {
	/* loadPalette() retouche les entrees noires (-> 0x8000), l'entree 1 et le bit 15 : on compare les couleurs sans ce bit, entrees 2.. */
	for (int i = 2; i < bytes / 2; i++) {
		const uint16 a = READ_LE_UINT16(cur + i * 2) & 0x7fff;
		const uint16 b = READ_LE_UINT16(ref + i * 2) & 0x7fff;
		if (a != b && (a > 1 || b > 1))   /* noir et quasi-noir (0x0001) sont confondus par loadPalette */
			return false;
	}
	return true;
}




#ifdef BD_SIM_DIAG
/* Outil (simulateur, BD_SPRBG=dossier) : pour chaque trame de personnage, quels pixels sont identiques au décor d'origine
   là où le jeu la dessine (personnages rectangulaires qui contiennent un morceau de décor : géant de s06a…).
   État par pixel : 0 jamais vu, 1 toujours égal au décor, 2 différent au moins une fois. Écrit à la sortie. */
namespace { struct SprBg { int w, h, kind; std::vector<uint8> st; int n; }; std::map<uint32, SprBg> g_sprBg; bool g_sprBgAt = false; }
static void sprBgDump() {
	const char *dir = getenv("BD_SPRBG");
	for (auto &e : g_sprBg) {
		char fn[512]; snprintf(fn, sizeof(fn), "%s/%d_%04x_%d.pgm", dir, (int)(e.first >> 16), (int)(e.first & 0xffff), e.second.kind);
		FILE *f = fopen(fn, "wb"); if (!f) continue;
		fprintf(f, "P5 %d %d 255\n", e.second.w, e.second.h);
		for (uint8 v : e.second.st) fputc(v == 1 ? 255 : (v == 2 ? 128 : 0), f);
		fclose(f);
	}
}
static void sprBgNote(Background *st, int act, uint16 fr, int kind, const Graphics::Surface &s, const byte *apal, const byte *spal, int wx, int wy, bool flip) {
	if (!st || kind < 0) return;
	if (!g_sprBgAt) { g_sprBgAt = true; atexit(sprBgDump); }
	const uint32 key = ((uint32)act << 16) | fr;
	SprBg &b = g_sprBg[key];
	if (b.st.empty()) { b.w = s.w; b.h = s.h; b.kind = kind; b.st.assign((size_t)s.w * s.h, 0); b.n = 0; }
	if (b.w != s.w || b.h != s.h) return;
	if (b.n == 0 && getenv("BD_SPRCTX")) {   /* contexte : décor d'origine autour de la trame (marge 16) avec la trame dessus, pour l'agrandir comme le décor */
		const int M = 16, W = s.w + 2 * M, H = s.h + 2 * M;
		std::vector<byte> rgb((size_t)W * H * 3, 0);
		Graphics::Surface *L3[3] = { st->getBgLayer(), st->getMgLayer(), st->getFgLayer() };
		for (int j = 0; j < H; j++) for (int i = 0; i < W; i++) {
			const int lx = wx - M + i, ly = wy - M + j;
			int c = -1;
			if (i >= M && j >= M && i < M + s.w && j < M + s.h) {
				const byte pi = ((const byte *)s.getBasePtr(flip ? s.w - 1 - (i - M) : i - M, j - M))[0];
				if (pi) c = READ_LE_UINT16(apal + pi * 2) & 0x7fff;
			}
			for (int k = 2; k >= 0 && c < 0; k--) {
				Graphics::Surface *l = L3[k];
				if (!l || lx < 0 || ly < 0 || lx >= l->w || ly >= l->h) continue;
				const byte li = ((const byte *)l->getBasePtr(lx, ly))[0];
				if (li) c = READ_LE_UINT16(spal + li * 2) & 0x7fff;
			}
			if (c < 0) c = 0;
			byte *q = &rgb[((size_t)j * W + (flip ? W - 1 - i : i)) * 3];   /* rangé dans le sens de la trame (comme les trames HD), même si le jeu la dessine retournée */
			q[0] = ((c >> 10) & 31) * 255 / 31; q[1] = ((c >> 5) & 31) * 255 / 31; q[2] = (c & 31) * 255 / 31;
		}
		char fn[512]; snprintf(fn, sizeof(fn), "%s/ctx_%d_%04x_%d_%d_%d.ppm", getenv("BD_SPRBG"), act, fr, kind, wx, wy);
		FILE *f = fopen(fn, "wb");
		if (f) { fprintf(f, "P6 %d %d 255\n", W, H); fwrite(rgb.data(), 1, rgb.size(), f); fclose(f); }
	}
	b.n++;
	Graphics::Surface *L[3] = { st->getBgLayer(), st->getMgLayer(), st->getFgLayer() };
	for (int j = 0; j < s.h; j++) for (int i = 0; i < s.w; i++) {
		const byte pi = ((const byte *)s.getBasePtr(flip ? s.w - 1 - i : i, j))[0];
		uint8 &v = b.st[(size_t)j * s.w + (flip ? s.w - 1 - i : i)];   /* masque dans le sens de la trame */
		if (pi == 0) { v = 2; continue; }   /* transparent : déjà hors du rectangle */
		const uint16 c = READ_LE_UINT16(apal + pi * 2) & 0x7fff;
		bool eq = false;
		const int lx = wx + i, ly = wy + j;
		for (int k = 2; k >= 0; k--) {   /* couche visible la plus haute à cet endroit (comparer à une couche cachée ouvrirait un trou) */
			Graphics::Surface *l = L[k];
			if (!l || lx < 0 || ly < 0 || lx >= l->w || ly >= l->h || l->w < st->getWidth()) continue;
			const byte li = ((const byte *)l->getBasePtr(lx, ly))[0];
			if (li == 0) continue;
			eq = (READ_LE_UINT16(spal + li * 2) & 0x7fff) == c;
			break;
		}
		if (!eq) v = 2; else if (v == 0) v = 1;
	}
}
#endif

Scene::Scene(DragonsEngine *vm, Screen *screen, ScriptOpcodes *scriptOpcodes, ActorManager *actorManager, DragonRMS *dragonRMS, DragonINIResource *dragonINIResource, BackgroundResourceLoader *backgroundResourceLoader)
		: _vm(vm), _screen(screen), _scriptOpcodes(scriptOpcodes), _stage(nullptr), _actorManager(actorManager), _dragonRMS(dragonRMS), _dragonINIResource(dragonINIResource), _backgroundLoader(backgroundResourceLoader), _hint(nullptr) {
	_mapTransitionEffectSceneID = 2;
	_data_800633ee = 0;

	_currentSceneId = -1;
}
void Scene::loadScene(uint32 sceneId, uint32 cameraPointId) {
#ifdef BD_SIM_DIAG
	if (BdProf::envFlag("BD_SCENELOG")) BdProf::say("LOADSCENE %x cam %d flicker %d\n", sceneId, cameraPointId, _vm->_dragonINIResource->getFlickerRecord() ? _vm->_dragonINIResource->getFlickerRecord()->sceneId : -1);
#endif
	if (!_vm->isFlagSet(ENGINE_FLAG_40)) {
		_vm->fadeToBlack();
	}
	bool unkFlag2Set = _vm->isUnkFlagSet(ENGINE_UNK1_FLAG_2);
	bool flag8set = _vm->isFlagSet(ENGINE_FLAG_8);
	_vm->clearFlags(ENGINE_FLAG_8);
	_vm->setUnkFlags(ENGINE_UNK1_FLAG_2);

	for (int i = 0; i < 8; i++) {
		_vm->_paletteCyclingTbl[i].updateInterval = 0;
	}

	// TODO
	_vm->reset_screen_maybe();

	loadSceneData(sceneId, cameraPointId);

	if (flag8set) {
		_vm->setFlags(ENGINE_FLAG_8);
	}

	if (!_vm->isFlagSet(ENGINE_FLAG_8000000) && sceneId != 4) {
		_vm->_cursor->updateSequenceID((int16)_vm->_cursor->_sequenceID);
	}
	_vm->waitForFrames(2);
	_vm->fadeFromBlack();
	if (!unkFlag2Set) {
		_vm->clearUnkFlags(ENGINE_UNK1_FLAG_2);
	}
	_data_800633ee = 0;

	if (!(sceneId & 0x8000)) {
		byte *obd = _dragonRMS->getAfterSceneLoadedScript(sceneId);
		ScriptOpCall scriptOpCall(obd + 4, READ_LE_UINT32(obd));
		_scriptOpcodes->runScript(scriptOpCall);
	}
	DragonINI *ini = _dragonINIResource->getRecord(0xc4);
	ini->objectState = 0;

	/* port DC : le joueur est dans la scène, on lit en avance celles où il peut aller */
	if (!_hint) _hint = new SceneHint(_vm);
	_hint->onSceneLoaded((uint16)sceneId);
}

/* Diagnostic des entrées de salle : pour les 60 premières images, une lettre par image
   (H : couches en HD, O : au moins une couche en original hors zones modifiées, - : aucune couche dessinée). */
static int s_enterLeft = 0, s_enterHd = 0, s_enterOrig = 0, s_enterN = 0;
static char s_enterWhy = 'O';   /* raison du dernier original : N couche modifiée, P palette, A mode de mélange, X absente/échec */
static char s_enterLine[64];
static void enterFrameDone(const char *name) {
	if (s_enterLeft <= 0) return;
	s_enterLine[s_enterN++] = s_enterOrig ? s_enterWhy : (s_enterHd ? 'H' : '-');
	s_enterHd = s_enterOrig = 0;
	if (--s_enterLeft == 0 || s_enterN >= 60) {
		s_enterLine[s_enterN] = 0;
		BdProf::say("ENTREE %s : %s\n", name ? name : "?", s_enterLine);
		s_enterLeft = 0;
	}
}

void Scene::loadSceneData(uint32 sceneId, uint32 cameraPointId) {
	bool isUnkFlag2Set = _vm->isUnkFlagSet(ENGINE_UNK1_FLAG_2);

	_vm->setUnkFlags(ENGINE_UNK1_FLAG_2 | Dragons::ENGINE_UNK1_FLAG_8);

	for (int i = 0; i < _dragonINIResource->totalRecords(); i++) {
		DragonINI *ini = _dragonINIResource->getRecord(i);
		ini->counter = -1;
		ini->flags &= ~INI_FLAG_10;
	}

	uint16 sceneIdStripped = (uint16)sceneId & ~0x8000;
	if (sceneIdStripped == 0x18 || sceneIdStripped == 0x26 || sceneIdStripped == 0x7 ||
			sceneIdStripped == 0x17 || sceneIdStripped == 0x5 || sceneIdStripped == 0x19 ||
			sceneIdStripped == 0x34 || sceneIdStripped == 0x1d || sceneIdStripped == 0x6) {
//		buf2048bytes = buf2048bytes + 0x1800;
		// error("0x8002f404"); //TODO do we need this logic?
	}

	_screen->setScreenShakeOffset(0, 0);

	if (!(sceneId & 0x8000)) {
		byte *obd = _dragonRMS->getBeforeSceneDataLoadedScript(sceneId);
		ScriptOpCall scriptOpCall(obd + 4, READ_LE_UINT32(obd));
		uint16 oldSceneId = _currentSceneId;
		_currentSceneId = -1;
		_scriptOpcodes->runScript(scriptOpCall);
		_currentSceneId = oldSceneId;
	}

	_actorManager->clearActorFlags(2);
	//TODO stopAndCloseSceneVab()

	_vm->_cursor->setActorFlag400();
	_vm->_inventory->setActorFlag400();
	_vm->clearFlags(ENGINE_FLAG_200);

	resetActorFrameFlags();

	// Loading animation logic would go here. 0x8002f538

	_vm->clearFlags(ENGINE_FLAG_20);
	_vm->setUnkFlags(ENGINE_UNK1_FLAG_10);

	_vm->fadeFromBlack();
	// TODO 0x8002f7c4

	_vm->_cursor->updatePosition(160, 100);

	_vm->clearFlags(ENGINE_FLAG_100000);
	_vm->clearFlags(ENGINE_FLAG_200000);

	DragonINI *flicker = _vm->_dragonINIResource->getFlickerRecord();

	if (flicker == nullptr || flicker->sceneId == 0) {
		_vm->getINI(1)->sceneId = 0;
	} else {
		_currentSceneId = (uint16)(sceneId & 0x7fff);
		flicker->sceneId = _currentSceneId;
		_vm->getINI(1)->sceneId = _currentSceneId;
	}

	_vm->loadCurrentSceneMsf();

	delete _stage;   /* port DC : fuite du décor précédent */
	_stage = _backgroundLoader->load(sceneId);
	if (!_vm->isFlagSet(ENGINE_FLAG_800)) {
		byte *cursorPalette = _vm->_cursor->getPalette();
		byte *stagePalette = _stage->getPalette();
		for (int i = 0xc0; i < 0x100; i++) {
			stagePalette[i * 2] = cursorPalette[(i-0xc0) * 2];
			stagePalette[i * 2 + 1] = cursorPalette[(i-0xc0) * 2 + 1];
		}
	}
	for (int i = 1; i < 0x100; i ++) {
		byte *stagePalette = _stage->getPalette();
		uint16 c = READ_LE_INT16(stagePalette + i * 2);
		if ((c & 0x7fff) == 0) {
			stagePalette[i * 2 + 1] |= 0x80;
		}
	}
	_screen->loadPalette(0, _stage->getPalette());

	for (int i = 1; i < 0x100; i ++) {
		byte *stagePalette = _stage->getPalette();
		uint16 c = READ_LE_INT16(stagePalette + i * 2);
		if ((c & 0x7fff) == 0) {
			stagePalette[i * 2] = 1;
			stagePalette[i * 2 + 1] = 0;
		}
	}

	_camera = _stage->getPoint2(cameraPointId);

	if (flicker && !(sceneId & 0x8000)) {
		flicker->x = _camera.x;
		flicker->y = _camera.y;
		_vm->getINI(1)->x = _camera.x;
		_vm->getINI(1)->y = _camera.y;
	}

	debug(3, "Flicker: (%X, %X)", _camera.x, _camera.y);

	if (_camera.x > 160) {
		_camera.x -= 160;
	} else {
		_camera.x = 0;
	}

	if (_camera.y > 100) {
		_camera.y -= 100;
	} else {
		_camera.y = 0;
	}

	if (_camera.x + 320 >= _stage->getWidth()) {
		_camera.x = _stage->getWidth() - 320;
	}

	if (_camera.y + 200 >= _stage->getHeight()) {
		_camera.y = _stage->getHeight() - 200;
	}

	debug(3, "Camera: (%d, %d)", _camera.x, _camera.y);

	// 0x8002ff80
	_vm->fadeToBlack();
	_vm->clearUnkFlags(ENGINE_UNK1_FLAG_10);
	_vm->setFlags(ENGINE_FLAG_20);
	// TODO reset vsync_updater_function

	_vm->setFlags(ENGINE_FLAG_200);
	_actorManager->clearActorFlags(2);
	_vm->_isLoadingDialogAudio = false;
	// TODO 0x8002fff0

	for (int i = 0; i < _dragonINIResource->totalRecords(); i++) {
		DragonINI *ini = _dragonINIResource->getRecord(i);
		if (ini->sceneId == sceneIdStripped) {
			if (ini->flags & 1) {
				Actor *actor = _actorManager->loadActor(ini->actorResourceId, ini->sequenceId, ini->x, ini->y, 0);

				if (actor) {
					ini->actor = actor;
					if (ini->flags & 0x1000) {
						actor->_frame_flags |= 0x10;
					} else {
						if (ini->flags & 0x2000) {
							actor->_frame_flags |= 0x20;
						} else {
							actor->_frame_flags &= ~0x10;
						}
					}

					actor->_direction = ini->direction2;

					if (ini->flags & 2) {
						actor->_flags |= ACTOR_FLAG_80;
					} else {
						actor->_flags &= 0xfeff;
					}

					if (ini->flags & 0x20) {
						actor->_flags |= ACTOR_FLAG_100;
					} else {
						actor->_flags &= 0xfeff;
					}

					if (ini->flags & 4) {
						actor->_flags |= ACTOR_FLAG_8000;
					} else {
						actor->_flags &= 0x7fff;
					}

					if (ini->flags & 0x100) {
						actor->_flags |= ACTOR_FLAG_4000;
					} else {
						actor->_flags &= 0xbfff;
					}
//
//				Graphics::Surface *s = actor->getCurrentFrame();
//				int x = ini->x - actor->_frame_vram_x;
//				int y = ini->y - actor->_frame_vram_y;
//				if (x >= 0 && y >= 0 && x + s->w < 320 && y + s->h < 200) {
//					debug("Actor %d, %d %d (%d, %d)", actor->_actorID, ini->actorResourceId, ini->flags, ini->x, ini->y);
//					_stage->getFgLayer()->copyRectToSurface(*s, x, y, Common::Rect(s->w, s->h));
//				}
				}
				// _stage->getFgLayer()->drawLine(ini->x, ini->y, ini->x + 8, ini->y + 8, 0x7c00);
				//break;
			} else {
				if (ini->iptIndex_maybe != -1) {
					loadImageOverlay(ini->iptIndex_maybe);
				}
			}
		}
	}

	// 0x80030458
	DragonINI *ini = _vm->getINI(1);
	if (ini->actor && _vm->_dragonINIResource->getFlickerRecord() && _vm->_dragonINIResource->getFlickerRecord()->sceneId == _currentSceneId) {
		ini->actor->setFlag(ACTOR_FLAG_100);
		ini->actor->_priorityLayer = 0;
	}


	if (flicker && flicker->sceneId != 0) {
		flicker->direction2 = _vm->_flickerInitialSceneDirection;
		if (flicker->actor) {
			flicker->actor->_direction = _vm->_flickerInitialSceneDirection;
			flicker->actor->setFlag(ACTOR_FLAG_4);
		}
	}

	// 0x800305bc
	_vm->_inventory->loadScene(_currentSceneId);

	// 0x8003070c
	// TODO sub_80013b3c(); // palette related.

	if (_vm->_inventory->isOpen()) {
		_vm->_inventory->close();
	}

	if (!_vm->isFlagSet(ENGINE_FLAG_10000)) {
		_vm->setFlags(ENGINE_FLAG_10);
	}

	_vm->setFlags(ENGINE_FLAG_1);
	_vm->setFlags(ENGINE_FLAG_200);
	_vm->setFlags(ENGINE_FLAG_4000000);

	if (flicker && flicker->sceneId == _currentSceneId) {

		flicker->actor->updateSequence((uint16)flicker->actor->_direction);
	}

	_vm->clearUnkFlags(ENGINE_UNK1_FLAG_2);
	_vm->clearUnkFlags(ENGINE_UNK1_FLAG_8);

	if (isUnkFlag2Set) {
		_vm->setUnkFlags(ENGINE_UNK1_FLAG_2);
	}

	if (!(sceneId & 0x8000)) {
		byte *obd = _dragonRMS->getAfterSceneDataLoadedScript(sceneId);
		ScriptOpCall scriptOpCall(obd + 4, READ_LE_UINT32(obd));
		_scriptOpcodes->runScript(scriptOpCall);
	}

	s_enterLeft = 60; s_enterN = 0; s_enterHd = s_enterOrig = 0;   /* diagnostic : voir enterFrameDone */
}

void Scene::draw() {
	if (s_enterN > 0 || s_enterLeft > 0) enterFrameDone(_stage ? _stage->getName() : nullptr);
	{   /* diagnostic décors larges : position de caméra quand elle change de 32 px ou plus */
		static int lastCam = -1000;
		if (_stage && _stage->getWidth() > 320 && abs(_camera.x - lastCam) >= 32) {
			lastCam = _camera.x;
			if (BdProf::envFlag("BD_CAMDBG")) BdProf::say("CAMERA %s x %d (decor %d, fond %d, plans %d/%d)\n", _stage->getName(), _camera.x, _stage->getWidth(),
			            _stage->getBgLayer() ? _stage->getBgLayer()->w : 0, _stage->getMgLayer() ? _stage->getMgLayer()->w : 0, _stage->getFgLayer() ? _stage->getFgLayer()->w : 0);
		}
	}
	Common::Rect rect(_camera.x, _camera.y, _camera.x + 320, _camera.y + 200);
	_vm->_screen->clearScreen();

	for (uint16 priority = 1; priority < 16; priority++) {
		if (priority == _vm->_overlayPriority && _vm->_overlay)    /* image de menu (ecran Controles) : sous le texte */
			_screen->copyRectToSurface(*_vm->_overlay, 0, 0);
		/* Menus : le texte était redessiné à chacune des 15 priorités (15 fois par image, 30 ms sur Dreamcast).
		   Une seule fois, à la dernière (celle qui restait au-dessus), avec le fond semi-transparent posé deux fois :
		   l'assombrissement est alors presque celui des 15 passages (il sature). */
		if (_vm->isInMenu() && priority == 15) {
			_vm->_fontManager->updatePalette();
			_vm->_fontManager->draw(2);
		} else if (!_vm->isInMenu() && priority == 7 && _vm->isFlagSet(ENGINE_FLAG_200)) {
			_vm->_fontManager->updatePalette();
			_vm->_fontManager->draw();
		}

		if (_vm->isFlagSet(ENGINE_FLAG_200)) {
			if (priority == 5) {
				if (_vm->isFlagSet(ENGINE_FLAG_80)) {
					_vm->_inventory->draw();
				}
			}

			if (priority == _stage->getFgLayerPriority()) {
				drawBgLayer(2, rect, _stage->getFgLayer());
			}
			if (priority == _stage->getMgLayerPriority()) {
				drawBgLayer(1, rect, _stage->getMgLayer());
			}
			if (priority == _stage->getBgLayerPriority()) {
				drawBgLayer(0, rect, _stage->getBgLayer());
			}
		}

		_screen->drawFlatQuads(priority);

		for (int16 i = 0; i < DRAGONS_ENGINE_NUM_ACTORS; i++) {
			Actor *actor = _actorManager->getActorByDisplayOrder(i);
			if (actor->_x_pos == -100 && actor->_y_pos == 100) {
				actor->_priorityLayer = 0;
				continue;
			}

			if (actor->_flags & ACTOR_FLAG_40 &&
					!(actor->_flags & ACTOR_FLAG_400) &&
					actor->_surface &&
					actor->_frame->width != 0 &&
					actor->_frame->height != 0) {
				Graphics::Surface *s = actor->_surface;
				if (actor->_priorityLayer == priority) { //} && x + s->w < 320 && y + s->h < 200) {
					if (!actor->isFlagSet(ACTOR_FLAG_80)) {
						actor->_scale = _stage->getScaleLayer()->getScale(actor->_y_pos);
					}
					int x = actor->_x_pos - (actor->_frame->xOffset * actor->_scale / DRAGONS_ENGINE_SPRITE_100_PERCENT_SCALE) - (actor->isFlagSet(ACTOR_FLAG_200) ? 0 : _camera.x);
					int y = actor->_y_pos - (actor->_frame->yOffset * actor->_scale / DRAGONS_ENGINE_SPRITE_100_PERCENT_SCALE) - (actor->isFlagSet(ACTOR_FLAG_200) ? 0 : _camera.y);

					debug(5, "Actor %d %s (%d, %d) w:%d h:%d Priority: %d Scale: %d", actor->_actorID, actor->_actorResource->getFilename(), x,
						  y,
						  s->w, s->h, actor->_priorityLayer, actor->_scale);
					const bool flip = (bool)(actor->_frame->flags & FRAME_FLAG_FLIP_X);
					const AlphaBlendMode am = actor->isFlagSet(ACTOR_FLAG_8000) ? NONE : NORMAL;
					byte *apal = actor->getPalette();
					/* sprite HD : genre de palette 1 (commune), 2 (celle du décor) ou 0 (propre), celles des images HD ; sinon l'original */
					const int pkind0 = (apal == _screen->getPalette(1)) ? 1 : (apal == _screen->getPalette(0)) ? 2 : (apal == actor->_actorResource->getPalette() ? 0 : -1);
					if (g_hdGraphics && g_renderBackend && pkind0 < 0) { g_hdSprPalMiss++; if (apal == _screen->getPalette(0)) g_hdSprPal0++; else if (apal == _screen->getPalette(4)) g_hdSprPal4++; }
					int pkind = pkind0;
					/* palette du décor dont les premières couleurs sont celles du sprite (Sir George dans s09i) : genre 0 ou 1 */
					if (pkind == 2 && _stage && !spritePaletteOk(apal, _stage->getPalette(), s)) {
						if (actor->_actorResource->getPalette() && spritePaletteOk(apal, actor->_actorResource->getPalette(), s)) pkind = 0;
						else if (_vm->_cursor && _vm->_cursor->getPalette() && spritePaletteOk(apal, _vm->_cursor->getPalette(), s)) pkind = 1;
					}
					const bool palOk = pkind == 2 ? (_stage && spritePaletteOk(apal, _stage->getPalette(), s)) : pkind == 1 ? (g_hdPal1RefOn ? spritePaletteOk(apal, g_hdPal1Ref, s) : _vm->_cursor && _vm->_cursor->getPalette() && (pkind0 == 2 || paletteIntact(apal, _vm->_cursor->getPalette(), 256) || spritePaletteOk(apal, _vm->_cursor->getPalette(), s))) : true;
					if (x > 140 && x < 240 && y > 170 && y < 215 && !strncmp(_stage->getName(), "s01j", 4)) { g_hdWhite[g_hdWhiteN % 8][0] = (int)actor->_actorResource->_id; g_hdWhite[g_hdWhiteN % 8][1] = actor->_frameOffset; g_hdWhite[g_hdWhiteN % 8][2] = pkind; g_hdWhite[g_hdWhiteN % 8][3] = palOk; g_hdWhite[g_hdWhiteN % 8][4] = x; g_hdWhite[g_hdWhiteN % 8][5] = y; g_hdWhiteN++; }
#ifdef BD_SIM_DIAG
					if (BdProf::envFlag("BD_SPRBG") && actor->_scale == DRAGONS_ENGINE_SPRITE_100_PERCENT_SCALE && s->format.bytesPerPixel == 1)
						sprBgNote(_stage, (int)actor->_actorResource->_id, actor->_frameOffset, pkind, *s, apal, _screen->getPalette(0),
						          x + (actor->isFlagSet(ACTOR_FLAG_200) ? 0 : _camera.x), y + (actor->isFlagSet(ACTOR_FLAG_200) ? 0 : _camera.y), flip);
#endif
					g_hdSprExpectW = s->w; g_hdSprExpectH = s->h;
					bool hdOk = g_hdGraphics && g_renderBackend && pkind >= 0 && palOk &&
							g_renderBackend->hdSprite((int)actor->_actorResource->_id, actor->_frameOffset, pkind0 == 2 ? 2 : pkind, x, y, flip, am, actor->_scale);
#ifdef BD_SIM_DIAG
					if (BdProf::envFlag("BD_ACTDBG") && !strncmp(_stage->getName(), getenv("BD_ACTDBG"), 4))
						BdProf::say("ACT %s fr %04x pk %d pal %d hd %d x %d y %d w %d h %d flip %d sc %d\n", actor->_actorResource->getFilename(), actor->_frameOffset, pkind, (int)palOk, (int)hdOk, x, y, s->w, s->h, (int)flip, actor->_scale);
#endif
					if (g_hdGraphics && g_renderBackend && !hdOk) {   /* journal : sprite dessiné en original alors que la HD est active */
						static uint32 seen[64]; static int nSeen = 0;
						const uint32 key = ((uint32)actor->_actorResource->_id << 16) | actor->_frameOffset;
						bool known = false; for (int k = 0; k < nSeen; k++) if (seen[k] == key) { known = true; break; }
						if (!known && nSeen < 64) {
							seen[nSeen++] = key;
							BdProf::say("SD %s %s trame %04x : palette genre %d ok %d, refus %d, x %d y %d\n", (_stage && _stage->getName()) ? _stage->getName() : "-", actor->_actorResource->getFilename() ? actor->_actorResource->getFilename() : "-",
							            actor->_frameOffset, pkind, (int)palOk, pkind >= 0 && palOk ? g_hdSprWhy : 0, x, y);
						}
						g_hdSprWhy = 0;
					}
					if (hdOk) {
						// dessiné en HD
					} else
					_screen->copyRectToSurface8bpp(*s, apal, x, y, Common::Rect(s->w, s->h), flip, am, actor->_scale);
					if (_vm->isDebugMode()) {
						_screen->drawRect(0x7fff, Common::Rect(x, y, x + s->w, y + s->h), actor->_actorID);
						drawActorNumber(x + s->w, y + 8, actor->_actorID);
					}
				}
			}
		}
	}
	if (_vm->_credits->isRunning()) {
		_vm->_credits->draw();
	}

	if (_vm->isDebugMode()) {
		_vm->_fontManager->clearText();
	}
}

int16 Scene::getPriorityAtPosition(Common::Point pos) {
	return _stage->getPriorityAtPoint(pos);
}

bool Scene::contains(DragonINI *ini) {
	assert(ini);
	return ini->sceneId == _currentSceneId;
}

byte *Scene::getPalette() {
	assert(_stage);
	return _stage->getPalette();
}

uint16 Scene::getSceneId() {
	return (uint16)_currentSceneId;
}

Common::Point Scene::getPoint(uint32 pointIndex) {
	return _stage->getPoint2(pointIndex);
}

uint16 Scene::getStageWidth() {
	return _stage->getWidth();
}

uint16 Scene::getStageHeight() {
	return _stage->getHeight();
}

void Scene::loadImageOverlay(uint16 iptId) {
	Img *img =_vm->_dragonImg->getImg(iptId);
	if (img->h != 0) {
		if (img->field_e <= 2) {
			_stage->overlayImage(img->layerNum - 1, img->data, img->x, img->y, img->w, img->h, (int16)iptId);
		}

		if (img->field_e == 2 || img->field_e == 0) {
			_stage->overlayPriorityTileMap(img->data + img->w * img->h * 2, img->x, img->y, img->w, img->h);
		}
	}
}

void Scene::removeImageOverlay(uint16 iptId) {
	Img *img =_vm->_dragonImg->getImg(iptId);
	_stage->restoreTiles(img->layerNum - 1, img->x, img->y, img->w, img->h);
	_stage->restorePriorityTileMap(img->x, img->y, img->w, img->h);
}

void Scene::setSceneId(int16 newSceneId) {
	_currentSceneId = newSceneId;
}

void Scene::resetActorFrameFlags() {
	for (int i = 0; i < 0x17; i++) {
		Actor *actor = _vm->_actorManager->getActor(i);
		actor->_frame_flags &= ~ACTOR_FRAME_FLAG_10;
		actor->_frame_flags &= ~ACTOR_FRAME_FLAG_20;
	}
}

void Scene::setBgLayerPriority(uint8 newPriority) {
	_stage->setBgLayerPriority(newPriority);
}

void Scene::setMgLayerPriority(uint8 newPriority) {
	_stage->setMgLayerPriority(newPriority);
}

void Scene::setFgLayerPriority(uint8 newPriority) {
	_stage->setFgLayerPriority(newPriority);
}

void Scene::setStagePalette(byte *newPalette) {
	_stage->setPalette(newPalette);
}


void Scene::drawActorNumber(int16 x, int16 y, uint16 actorId) {
	uint16 text[30];
	char text8[15];

	Common::sprintf_s(text8, "%d", actorId);

	for (uint i = 0; i < strlen(text8); i++) {
		text[i] = text8[i];
	}
	_vm->_fontManager->addText(x, y, text, strlen(text8), 1);
}

void Scene::setLayerOffset(uint8 layerNumber, Common::Point offset) {
	_stage->setLayerOffset(layerNumber, offset);
}

Common::Point Scene::getLayerOffset(uint8 layerNumber) {
	return _stage->getLayerOffset(layerNumber);
}

void Scene::drawBgLayer(uint8 layerNumber, Common::Rect rect, Graphics::Surface *surface) {
	/* Couche plus étroite que le décor (fond de s12a : 448 pour 480) : elle défile moins vite (parallaxe) au lieu de laisser
	   une bande noire au bord droit quand la caméra arrive au bout. */
	{
		const int sw = _stage->getWidth(), lw = surface->w;
		if (lw < sw && sw > 320 && lw >= 320 && rect.left > 0) {
			const int nl = rect.left * (lw - 320) / (sw - 320);
			rect.right -= rect.left - nl; rect.left = nl;
		}
	}
	Common::Point offset = _stage->getLayerOffset(layerNumber);
#ifdef BD_SIM_DIAG
	if (BdProf::envFlag("BD_CAMDBG")) BdProf::say("CAM %s c%d cam %d,%d rect %d..%d off %d,%d w %d stageW %d\n", _stage->getName(), layerNumber, _camera.x, _camera.y, rect.left, rect.right, offset.x, offset.y, surface->w, _stage->getWidth());
#endif
//	Common::Rect clippedRect = _screen->clipRectToRect(offset.x, offset.y, rect, Common::Rect(_stage->getBgLayer()->w, _stage->getBgLayer()->h));
	rect.left += rect.left + offset.x < 0 ? -(rect.left + offset.x) : offset.x;
	if (rect.right + offset.x > surface->w) {
		rect.right = surface->w - 1;
	} else {
		rect.right += offset.x;
	}
//	clippedRect.right += offset.x < 0 ? -offset.x : 0;
	rect.top += rect.top + offset.y < 0 ? -(rect.top + offset.y) : offset.y;
	if (rect.bottom + offset.y > surface->h) {
		rect.bottom = surface->h - 1;
	} else {
		rect.bottom += offset.y;
	}
//	clippedRect.bottom += offset.y < 0 ? -offset.y : 0;
	/* Décor HD : seulement si la palette du décor n'a pas changé. Les zones modifiées par les scripts (objets posés ou retirés, overlayImage)
	   sont dessinées avec l'original, le reste de la couche en HD ; l'écran est découpé en rectangles (setLayerClip). */
	if (g_hdGraphics && g_renderBackend && _stage) {
		g_hdLayTry++;
		if (layerNumber == 0) s_bgPalOk = false;
		if (!_stage->isLayerHdOk(layerNumber)) { s_enterWhy = 'N'; g_hdLayNotPristine++; hdLayStatNote(_stage->getName(), layerNumber, false, (int)_stage->getLayerAlphaMode(layerNumber), 2); }
		else if (!paletteIntactUsed(_screen->getPalette(0), _stage->getPalette(), layerUsedColours(_stage, layerNumber, _stage->getLayerVersion(layerNumber), surface))
		         && !(layerNumber > 0 && s_bgPalOk)) {   /* fond intact : ce n'est pas un fondu, mais une animation des couleurs de cette couche (neige de s08a) */ s_enterWhy = 'P'; g_hdLayPal++; g_hdPalDiff[0]++; hdLayStatNote(_stage->getName(), layerNumber, false, (int)_stage->getLayerAlphaMode(layerNumber), 1); }
		else {
			if (layerNumber == 0) s_bgPalOk = true;
			const AlphaBlendMode am = _stage->getLayerAlphaMode(layerNumber);
			if (am != NONE && am != NORMAL) s_enterWhy = 'A';
			if (am == NONE || am == NORMAL) {
				const int lw = surface->w;
				const int left = ((rect.left % lw) + lw) % lw, top = rect.top > 0 ? rect.top : 0;
				const int winW = lw < DRAGONS_SCREEN_WIDTH ? lw : DRAGONS_SCREEN_WIDTH;
				const int winH = (rect.height() < surface->h - top) ? rect.height() : surface->h - top;
				enum { MAXR = 160 };
				Common::Rect dirty[48]; int nd = 0;
				/* Images posées par les scripts qui existent en HD (« o<ipt> » dans HD.DAT) : leur zone n'est plus dessinée en
				   original ; le décor HD est dessiné dessous, puis la version HD de l'image (pixels changés seulement) par-dessus. */
				Common::Rect src[16]; int ns = 0;
				/* « r<ipt><scène> » : image qui rend transparente une partie du décor (porte qui s'ouvre…) ; sa version HD
				   remplace la couche HD dans toute la zone (exclue du décor HD, comme une zone modifiée, mais sans original). */
				int hdOv[8]; bool hdRep[8]; char hdName[8][12]; int nHdOv = 0;
				/* décors larges (défilement) : pas de versions HD des images posées, leur position ne suit pas encore le recollage du décor */
				int nOvl = _stage->getOverlayCount(layerNumber);
#ifdef BD_SIM_DIAG
				if (BdProf::envFlag("BD_SKIPOVL")) nOvl = 0;   /* essai : décor HD seul, sans les images posées */
#endif
				for (int i = 0; i < nOvl && nHdOv < 8; i++) {
					const Background::OverlayNote &o = _stage->getOverlay(layerNumber, i);
					char *nm = hdName[nHdOv];
					/* « o<ipt><scène> » : la même image peut être posée dans plusieurs scènes (ipt 335 : s10a et s09h) ;
					   sa version HD n'a été faite qu'avec les couleurs et le décor d'une d'elles */
					snprintf(nm, 12, "o%03d%.4s", o.ipt % 1000, _stage->getName());
					bool ok = g_renderBackend->hdPatch(nm, layerNumber, 0, 0, false), rep = false;
					if (!ok) { snprintf(nm, 12, "r%03d%.4s", o.ipt % 1000, _stage->getName()); ok = rep = g_renderBackend->hdPatch(nm, layerNumber, 0, 0, false); }
					if (ok) { hdRep[nHdOv] = rep; hdOv[nHdOv++] = i; }
#ifdef BD_SIM_DIAG
					if (BdProf::envFlag("BD_DIRTYDBG")) BdProf::say("OVL %s c%d ipt %d rect %d,%d-%d,%d hd %d\n", _stage->getName(), layerNumber, o.ipt, o.r.left, o.r.top, o.r.right, o.r.bottom, (int)ok);
#endif
				}
				for (int i = 0; i < _stage->getDirtyCount(layerNumber) && ns < 16 && nOvl; i++) src[ns++] = _stage->getDirty(layerNumber, i);
				for (int k = 0; k < nHdOv; k++) {   /* zones modifiées moins celles des images HD */
					const Common::Rect c = _stage->getOverlay(layerNumber, hdOv[k]).r;
					Common::Rect out[16]; int no = 0; bool full = false;
					for (int i = 0; i < ns && !full; i++) {
						const Common::Rect d = src[i];
						if (d.right <= c.left || d.left >= c.right || d.bottom <= c.top || d.top >= c.bottom) { if (no < 16) out[no++] = d; else full = true; continue; }
						const int t = MAX(d.top, c.top), b = MIN(d.bottom, c.bottom);
						if (no + 4 > 16) { full = true; break; }
						if (d.top < c.top) out[no++] = Common::Rect(d.left, d.top, d.right, c.top);
						if (d.bottom > c.bottom) out[no++] = Common::Rect(d.left, c.bottom, d.right, d.bottom);
						if (d.left < c.left) out[no++] = Common::Rect(d.left, t, c.left, b);
						if (d.right > c.right) out[no++] = Common::Rect(c.right, t, d.right, b);
					}
					if (full) { nHdOv = k; break; }   /* trop morcelé : le reste en original */
					ns = no; for (int i = 0; i < ns; i++) src[i] = out[i];
				}
				for (int i = 0; i < ns; i++) {
					const Common::Rect d = src[i];
					for (int k = -1; k <= 1; k++) {
						int x0 = d.left * 32 + k * lw - left, x1 = d.right * 32 + k * lw - left, y0 = d.top * 8 - top, y1 = d.bottom * 8 - top;
						if (x0 < 0) x0 = 0; if (x1 > winW) x1 = winW; if (y0 < 0) y0 = 0; if (y1 > winH) y1 = winH;
						if (x0 < x1 && y0 < y1 && nd < 48) dirty[nd++] = Common::Rect(x0, y0, x1, y1);
					}
				}
				const int ndOrig = nd;   /* au-delà : zones des images « r » (exclues du décor HD, sans original) */
				for (int k = 0; k < nHdOv; k++) {
					if (!hdRep[k]) continue;
					const Common::Rect d = _stage->getOverlay(layerNumber, hdOv[k]).r;
					/* seuls les blocs 8x8 de l'image qui ont des pixels transparents (trous laissant voir derrière) sont retirés
					   du décor HD ; ailleurs le décor HD reste et l'image HD n'y dessine que ses pixels changés (même texture) */
					const int ndK = nd;
					bool over = false;
					for (int by = d.top * 8; by < d.bottom * 8 && !over; by += 8) {
						int run = -1;
						for (int bx = d.left * 32; bx <= d.right * 32; bx += 8) {
							bool hole = false;
							if (bx < d.right * 32 && bx < surface->w)
								for (int yy = by; yy < by + 8 && yy < surface->h && !hole; yy++) {
									const byte *row = (const byte *)surface->getBasePtr(bx, yy);
									for (int xx = 0; xx < 8 && bx + xx < surface->w; xx++) if (!row[xx]) { hole = true; break; }
								}
							if (hole && run < 0) run = bx;
							if (!hole && run >= 0) {
								int x0 = run - left, x1 = bx - left, y0 = by - top, y1 = by + 8 - top;
								if (x0 < 0) x0 = 0; if (x1 > winW) x1 = winW; if (y0 < 0) y0 = 0; if (y1 > winH) y1 = winH;
								if (x0 < x1 && y0 < y1) { if (nd < 48) dirty[nd++] = Common::Rect(x0, y0, x1, y1); else over = true; }
								run = -1;
							}
						}
					}
					if (over) nd = ndK;   /* trop de morceaux : trous non découpés (le décor HD reste entier) */
#ifdef BD_SIM_DIAG
					if (BdProf::envFlag("BD_DIRTYDBG")) BdProf::say("RHOLES %s c%d ipt %d morceaux %d%s\n", _stage->getName(), layerNumber, _stage->getOverlay(layerNumber, hdOv[k]).ipt, nd - ndK, over ? " TROP" : "");
#endif
				}
				Common::Rect piece[MAXR]; int np = 0; bool tooMany = false;
				if (winW > 0 && winH > 0) piece[np++] = Common::Rect(0, 0, winW, winH);
				/* Le décor HD n'est découpé que pour les zones « r » (remplacées). Les zones modifiées dessinées en original
				   le sont par-dessus le décor HD entier : découper le décor HD à leurs bords laissait des traits fins
				   (bords de morceaux tombant entre deux pixels en plein écran, x 2,4). */
				for (int i = ndOrig; i < nd && !tooMany; i++) {
					Common::Rect next[MAXR]; int nn = 0;
					const Common::Rect d = dirty[i];
					for (int j = 0; j < np; j++) {
						const Common::Rect p = piece[j];
						if (p.right <= d.left || p.left >= d.right || p.bottom <= d.top || p.top >= d.bottom) { if (nn < MAXR) next[nn++] = p; else tooMany = true; continue; }
						const int t = p.top > d.top ? p.top : d.top, b = p.bottom < d.bottom ? p.bottom : d.bottom;
						if (nn + 4 > MAXR) { tooMany = true; break; }
						if (p.top < d.top) next[nn++] = Common::Rect(p.left, p.top, p.right, d.top);
						if (p.bottom > d.bottom) next[nn++] = Common::Rect(p.left, d.bottom, p.right, p.bottom);
						if (p.left < d.left) next[nn++] = Common::Rect(p.left, t, d.left, b);
						if (p.right > d.right) next[nn++] = Common::Rect(d.right, t, p.right, b);
					}
					if (!tooMany) { np = nn; for (int j = 0; j < np; j++) piece[j] = next[j]; }
				}
				bool drawn = false;
				if (_stage->isLayerEmptyAtLoad(layerNumber)) drawn = !tooMany;   /* couche vide d'origine (absente de HD.DAT) : seules les images posées comptent */
				else if (!tooMany && np > 0) {
					/* images « r » d'une couche au-dessus du fond (escalier de s09h) : leurs pixels opaques reprennent ce que
					   montraient les couches de dessous ; on redessine ces couches HD (mêmes textures) dans la zone de l'image,
					   hors blocs à trous, pour cacher ce qui passe derrière ; l'image HD n'ajoute ensuite que ses pixels nouveaux */
					if (layerNumber > 0 && _stage->getWidth() <= DRAGONS_SCREEN_WIDTH) {
						for (int k = 0; k < nHdOv; k++) {
							if (!hdRep[k]) continue;
							const Common::Rect d = _stage->getOverlay(layerNumber, hdOv[k]).r;
							Common::Rect pc[MAXR]; int npc = 0; bool many = false;
							{ int x0 = d.left * 32 - left, x1 = d.right * 32 - left, y0 = d.top * 8 - top, y1 = d.bottom * 8 - top;
							  if (x0 < 0) x0 = 0; if (x1 > winW) x1 = winW; if (y0 < 0) y0 = 0; if (y1 > winH) y1 = winH;
							  if (x0 < x1 && y0 < y1) pc[npc++] = Common::Rect(x0, y0, x1, y1); }
							for (int i = ndOrig; i < nd && !many; i++) {
								Common::Rect nx[MAXR]; int nn = 0; const Common::Rect h = dirty[i];
								for (int j = 0; j < npc; j++) {
									const Common::Rect q = pc[j];
									if (q.right <= h.left || q.left >= h.right || q.bottom <= h.top || q.top >= h.bottom) { if (nn < MAXR) nx[nn++] = q; else many = true; continue; }
									const int t = MAX(q.top, h.top), b = MIN(q.bottom, h.bottom);
									if (nn + 4 > MAXR) { many = true; break; }
									if (q.top < h.top) nx[nn++] = Common::Rect(q.left, q.top, q.right, h.top);
									if (q.bottom > h.bottom) nx[nn++] = Common::Rect(q.left, h.bottom, q.right, q.bottom);
									if (q.left < h.left) nx[nn++] = Common::Rect(q.left, t, h.left, b);
									if (q.right > h.right) nx[nn++] = Common::Rect(h.right, t, q.right, b);
								}
								if (!many) { npc = nn; for (int j = 0; j < npc; j++) pc[j] = nx[j]; }
							}
							if (many) continue;
							for (int lo = 0; lo < layerNumber; lo++) {
								if (!_stage->isLayerHdOk(lo) || _stage->getLayerAlphaMode(lo) != NONE || _stage->isLayerEmptyAtLoad(lo)) continue;
								for (int j = 0; j < npc; j++) {
									g_renderBackend->setLayerClip(true, pc[j].left, pc[j].top, pc[j].right, pc[j].bottom);
									g_renderBackend->hdLayer(_stage->getName(), lo, rect, NONE);
								}
							}
							g_renderBackend->setLayerClip(false, 0, 0, 0, 0);
						}
					}
					for (int j = 0; j < np; j++) {
						g_renderBackend->setLayerClip(true, piece[j].left, piece[j].top, piece[j].right, piece[j].bottom);
						const bool ok = g_renderBackend->hdLayer(_stage->getName(), layerNumber, rect, am);
						if (j == 0 && !ok) break;   /* couche absente du pack ou non chargeable : original (rien n'a été dessiné) */
						drawn = true;
					}
					g_renderBackend->setLayerClip(false, 0, 0, 0, 0);
				}
				if (drawn) {
					for (int k = 0; k < nHdOv; k++) {
						const Background::OverlayNote &o = _stage->getOverlay(layerNumber, hdOv[k]);
						const char *nm = hdName[k];
						const int X0 = MAX(0, o.r.left * 32 - 16), Y0 = MAX(0, o.r.top * 8 - 16);   /* marge de 16 px de tools/ovl_dump.py */
						g_renderBackend->setLayerClip(true, 0, 0, winW, winH);
						g_renderBackend->hdPatch(nm, layerNumber, X0 - left, Y0 - top, true);
						g_renderBackend->setLayerClip(false, 0, 0, 0, 0);
					}
					s_enterHd++;
					g_hdLayHit++; hdLayStatNote(_stage->getName(), layerNumber, true, (int)_stage->getLayerAlphaMode(layerNumber));
#ifdef BD_SIM_DIAG
					if (BdProf::envFlag("BD_DIRTYDBG")) { int a = 0; for (int i = 0; i < nd; i++) a += dirty[i].width() * dirty[i].height(); BdProf::say("DIRTY %s c%d nd %d aire %d / %d\n", _stage->getName(), layerNumber, nd, a, winW * winH); }
#endif
#ifdef BD_SIM_DIAG
					if (s_palAnimN > 0 && BdProf::envFlag("BD_PALDBG")) { const byte *cp = _screen->getPalette(0); for (int i = 0; i < 256; i++) if (s_palAnim[i]) BdProf::say("PALANIM %s c%d i%d %04x\n", _stage->getName(), layerNumber, i, READ_LE_UINT16(cp + i * 2)); }
#endif
					if (s_palAnimN > 0) {   /* réactivé : le rectangle noir de s12a venait des tuiles HD évincées (corrigé dans hdPatch) */
						/* Animation de palette (eau…) : les pixels des couleurs animées, et eux seuls, sont redessinés depuis le décor
						   d'origine avec les couleurs du moment (palette où toutes les autres couleurs sont transparentes), lissés par
						   le filtrage de la carte graphique. La texture n'est refaite que quand la palette change. */
						byte animPal[512];
						const byte *cp = _screen->getPalette(0);
						for (int i = 0; i < 256; i++) {
							const uint16 c = READ_LE_UINT16(cp + i * 2);
							const uint16 v = s_palAnim[i] ? c : 0;   /* couleur 0 = transparente (neige : flocon absent) ; bit 15 d’origine gardé (sinon tout devient demi-transparent et la couche est reconstruite à chaque image) */
							WRITE_LE_UINT16(animPal + i * 2, v);
						}
						_screen->copyRectToSurface8bppWrappedX(*surface, animPal, rect, NORMAL, _stage->getLayerVersion(layerNumber));
					}
					if (ndOrig > 0) {   /* zones modifiées : l'original par-dessus */
						for (int i = 0; i < ndOrig; i++) {
							g_renderBackend->setLayerClip(true, dirty[i].left, dirty[i].top, dirty[i].right, dirty[i].bottom);
							_screen->copyRectToSurface8bppWrappedX(*surface, _screen->getPalette(0), rect, am, _stage->getLayerVersion(layerNumber));
						}
						g_renderBackend->setLayerClip(false, 0, 0, 0, 0);
					}
					return;
				}
				s_enterWhy = 'X'; g_hdLayFail++; hdLayStatNote(_stage->getName(), layerNumber, false, (int)_stage->getLayerAlphaMode(layerNumber)); strncpy(g_hdLayFailName, _stage->getName(), 4); g_hdLayFailName[4] = '0' + layerNumber; g_hdLayFailName[5] = 0;
			}
		}
	}
	s_enterOrig++;
#ifdef BD_SIM_DIAG
	if (BdProf::envFlag("BD_DIRTYDBG") && _stage) BdProf::say("ORIG %s c%d mode %d hdok %d pourquoi %c\n", _stage->getName(), layerNumber, (int)_stage->getLayerAlphaMode(layerNumber), (int)_stage->isLayerHdOk(layerNumber), s_enterWhy ? s_enterWhy : (char)0x2d);
#endif
	_screen->copyRectToSurface8bppWrappedX(*surface, _screen->getPalette(0), rect, _stage->getLayerAlphaMode(layerNumber), _stage->getLayerVersion(layerNumber));
}

ScaleLayer *Scene::getScaleLayer() {
	return _stage->getScaleLayer();
}

void Scene::setLayerAlphaMode(uint8 layerNumber, AlphaBlendMode mode) {
	_stage->setLayerAlphaMode(layerNumber, mode);
}

} // End of namespace Dragons
