#define FORBIDDEN_SYMBOL_ALLOW_ALL
/* Lecture des vidéos STR (PlayStation) : décodeur MDEC maison (strdec) + son XA vers le mélangeur.
   Cadence : les secteurs CD sont lus à 150 par seconde (lecteur x2) ; chaque image est affichée à l'heure
   de son dernier secteur. Si le décodage prend du retard, les images suivantes sont sautées (le son
   continue). Sélection / Entrée passent la vidéo. */
#include "dragons/dragons.h"
#include "dragons/sound.h"
#include "dragons/strplayer.h"
#include "dragons/screen.h"
#include "common/events.h"
#include "common/system.h"
#include "graphics/surface.h"
#include "snd.h"
#include "subtitles.h"
#include "common/file.h"
#include "strdec.h"
#include "bdprof.h"
#include "labintro_map.inc"
#include <stdlib.h>
#include <stdio.h>

namespace Dragons {

StrPlayer::StrPlayer(DragonsEngine *vm, Screen *screen) : _vm(vm), _screen(screen), _decoder(nullptr) {}
StrPlayer::~StrPlayer() {}

static void audioCb(const uint8_t *xa, bool stereo, bool r18900, void *) {
	Snd::Mixer::get().streamPushXa(xa, stereo, r18900);
}

void StrPlayer::playVideo(const Common::Path &filename) {
	Common::String name = filename.toString();
	Str::Decoder dec;
	if (!dec.open(name.c_str())) { BdProf::say("STR : %s introuvable, video ignoree\n", name.c_str()); return; }
	int w = 0, h = 0;
	if (!dec.probeSize(&w, &h) || w > 640 || h > 480) { BdProf::say("STR : %s illisible\n", name.c_str()); return; }
	uint16_t *pix = (uint16_t *)malloc((size_t)w * h * 2);
	if (!pix) return;
	for (int i = 0; i < w * h; i++) pix[i] = 0;
	Graphics::Surface surf;
	surf.init(w, h, w * 2, pix, _screen->getPixelFormat());
	/* centré ; si la vidéo dépasse l'écran 320x200 on rogne les bords (le moteur n'accepte que des copies incluses dans l'écran) */
	const int dx = w < 320 ? (320 - w) / 2 : 0, dy = h < 200 ? (200 - h) / 2 : 0;
	const int sx = w > 320 ? (w - 320) / 2 : 0, sy = h > 200 ? (h - 200) / 2 : 0;
	const Common::Rect src(sx, sy, sx + (w < 320 ? w : 320), sy + (h < 200 ? h : 200));

	/* sous-titres : <video>.sub (voir port/common/subtitles.h), seulement si l'option est active */
	Subs::Track subs; bool haveSubs = false;
	if (!_vm->isFlagSet(ENGINE_FLAG_1000_SUBTITLES_DISABLED)) {
		Common::String base = name;
		int dot = -1; for (int i = (int)base.size() - 1; i >= 0; i--) if (base[i] == '.') { dot = i; break; }
		if (dot >= 0) base = base.substr(0, dot);
		Common::File sf;
		if (sf.open(Common::Path(base + ".sub"))) {
			int64 sz = sf.size();
			if (sz > 0 && sz < 65536) {
				char *buf = (char *)malloc((size_t)sz + 1);
				if (buf) { sf.read(buf, (uint32)sz); haveSubs = subs.parse(buf, (size_t)sz); free(buf); }
			}
			sf.close();
		}
		if (haveSubs) BdProf::say("STR : %s %d sous-titres\n", name.c_str(), subs.count());
	}
	const uint16 subFg = (uint16)_screen->getPixelFormat().RGBToColor(255, 255, 255), subOut = (uint16)_screen->getPixelFormat().RGBToColor(0, 0, 0);
	const int vw = w < 320 ? w : 320, vh = h < 200 ? h : 200;

	bool haveAudio = Snd::Mixer::get().streamBegin();
	Snd::Mixer::get().statStrUnder = 0;
	if (haveAudio) dec.setAudio(audioCb, nullptr);

	/* labintro.str en HD (mêmes 742 images en version France et USA, seules les voix changent), quel que soit le choix Graphismes (l intro passe avant la lecture des réglages) : chaque image de la vidéo est remplacée par sa version HD (images fixes « Lnnn » de HD.DAT,
	   la vidéo n'a que quelques images différentes par seconde) ; le son et la cadence restent ceux de la vidéo. */
	bool hdIntro = false;
	{
		Common::String lower = name; lower.toLowercase();
		if (g_renderBackend && lower.contains("labintro") && g_renderBackend->hdPatch("L000", 0, 0, 0, false)) hdIntro = true;
	}
	const uint32 t0 = g_system->getMillis();
	uint32 lastDue = 0, frames = 0, shown = 0, skipped = 0, msDecode = 0, msShow = 0, nDecoded = 0;
	bool quit = false, late = false; int lateCount = 0;
	while (!_vm->shouldQuit() && !quit) {
		uint32 tA = g_system->getMillis();
		int r = dec.nextFrame(late ? nullptr : pix, w);
		if (!late) { msDecode += g_system->getMillis() - tA; nDecoded++; }
		if (r <= 0) break;
		frames++;
#ifdef BD_SIM_DIAG
		if (getenv("BD_STRDUMP") && !late) {   /* outil : images de la vidéo (PPM), pour leur version HD */
			char fn[512]; snprintf(fn, sizeof(fn), "%s/%s_%04u.ppm", getenv("BD_STRDUMP"), name.c_str(), (unsigned)frames);
			FILE *f = fopen(fn, "wb");
			if (f) { fprintf(f, "P6 %d %d 255\n", w, h);
				for (int i = 0; i < w * h; i++) { uint8 r, g, b; _screen->getPixelFormat().colorToRGB(pix[i], r, g, b); fputc(r, f); fputc(g, f); fputc(b, f); }
				fclose(f); }
		}
#endif
		uint32 due = (uint32)((uint64_t)dec.sectorsRead() * 1000 / 150);
		/* attendre l'heure d'affichage en surveillant les touches */
		for (;;) {
			Common::Event event;
			while (g_system->getEventManager()->pollEvent(event)) {
				if (event.type == Common::EVENT_CUSTOM_ENGINE_ACTION_END
					&& (event.customType == Dragons::kDragonsActionSelect || event.customType == Dragons::kDragonsActionEnter || event.customType == Dragons::kDragonsActionMenu))
					quit = true;
			}
			uint32 now = g_system->getMillis() - t0;
			if (quit || now >= due || _vm->shouldQuit()) break;
			g_system->delayMillis(due - now > 8 ? 4 : 1);
		}
		if (quit) break;
		uint32 now = g_system->getMillis() - t0;
		if (!late) {
			if (haveSubs) {   /* le texte est ecrit dans l'image (chaque image est decodee en entier : rien a effacer) */
				const char *t = subs.textAt(due);
				if (t) Subs::drawText(pix + (size_t)sy * w + sx, w, vw, vh, t, subFg, subOut);
			}
			uint32 tB = g_system->getMillis();
			_screen->clearScreen();
			bool drawnHd = false;
			if (hdIntro && frames >= 1 && frames <= sizeof(kLabIntroMap)) {
				char nm[8]; snprintf(nm, sizeof(nm), "L%03u", (unsigned)kLabIntroMap[frames - 1]);
				drawnHd = g_renderBackend->hdPatch(nm, 0, dx - sx, dy - sy, true);
			}
			if (!drawnHd) _screen->copyVideoFrame(surf, dx, dy, src);
			_screen->updateScreen();
			msShow += g_system->getMillis() - tB;
			shown++;
		} else {
			skipped++;
		}
		late = (now > due + 30);                 /* en retard : sauter le décodage de l'image suivante */
		if (late && ++lateCount == 4) dec.setSmooth(false);   /* machine trop lente : chrominance au plus simple */
		lastDue = due;
	}
	(void)lastDue;
	if (haveAudio) {
		if (quit || _vm->shouldQuit()) Snd::Mixer::get().streamStop();
		else {
			Snd::Mixer::get().streamEnd();
			uint32 w0 = g_system->getMillis();
			while (Snd::Mixer::get().streamActive() && g_system->getMillis() - w0 < 3000 && !_vm->shouldQuit()) g_system->delayMillis(20);
			Snd::Mixer::get().streamStop();
		}
	}
	if (haveAudio) BdProf::say("STR : %s son video : %u coupures (tampon vide)\n", name.c_str(), (unsigned)Snd::Mixer::get().statStrUnder);
	BdProf::say("STR : %s %dx%d, %u images lues, %u affichees, %u sautees, %u secteurs ; decodage %u ms/image (lecture disque comprise), affichage %u ms/image\n", name.c_str(), w, h, (unsigned)frames, (unsigned)shown, (unsigned)skipped, (unsigned)dec.sectorsRead(), nDecoded ? (unsigned)(msDecode / nDecoded) : 0u, shown ? (unsigned)(msShow / shown) : 0u);
	_screen->clearScreen();
	_screen->updateScreen();
	dec.close();
	free(pix);
}

}
