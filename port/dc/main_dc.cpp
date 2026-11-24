/* Point d'entrée Dreamcast (KOS).
 *
 * Affichage : rendu direct par l'API PVR de KallistiOS (voir gpu_sink_dc.h) :
 * décor en couches et sprites en quads texturés, écran 320×200 agrandi ×2
 * (640×400, centré dans 640×480), décors et sprites HD en textures VQ.
 *
 * Manette : croix directionnelle = haut/bas/gauche/droite (actions PS1),
 *           stick analogique et souris = curseur,
 *           A = sélectionner (clic gauche), B = changer d'action (clic droit),
 *           Y = inventaire, START = entrée, X = croix PS1 (mini-jeux),
 *           L / R = L1 / R1.
 */
#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "bdprof.h"
#include "audio_dc.h"
#include "disc.h"
#include "inputmap.h"
#include "prefetch.h"
#include <kos.h>
#include <dc/vmu_pkg.h>
#include <dc/vmufs.h>
#include "vmuapi.h"
#include "snd.h"
#include <malloc.h>
#include <unistd.h>
#include <dirent.h>
#include <deque>
#include "common/system.h"
#include "common/events.h"
#include "common/savefile.h"
#include "common/memstream.h"
#include "common/file.h"
#include "common/config-manager.h"
#include "dragons/dragons.h"
#include "dragons/detection.h"
#include "dragons/screen.h"
#ifdef BD_GPU
#include "gpu_core.h"
#include "gpu_sink_dc.h"

/* Langue du disque : DRAGON.EXE de la version USA (368 640 octets) -> anglais, sinon français (version France, 372 736) */
static Common::Language bdDetectLanguage(const char *dir) {
	char p[512]; snprintf(p, sizeof(p), "%s/DRAGON.EXE", dir);
	FILE *f = fopen(p, "rb");
	if (!f) { snprintf(p, sizeof(p), "%s/dragon.exe", dir); f = fopen(p, "rb"); }
	if (!f) return Common::FR_FRA;
	fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f);
	return n == 368640 ? Common::EN_USA : Common::FR_FRA;
}
static Gpu::HdSprPack *g_hdSprPackPtr = nullptr;   /* statistiques de l'index des sprites HD */
#endif

#ifndef BD_BUILD
#define BD_BUILD "inconnue"
#endif
KOS_INIT_FLAGS(INIT_DEFAULT);

enum { W = 320, H = 200, TW = 512, TH = 256 };

class DcEvents : public Common::EventManager {
public:
	std::deque<Common::Event> queue;
	bool pollEvent(Common::Event &e) override {
		if (queue.empty()) return false;
		e = queue.front(); queue.pop_front();
		return true;
	}
	void pushEvent(const Common::Event &e) override { queue.push_back(e); }
};

/* Sauvegardes sur VMU : un fichier par emplacement (BLAZDRAG001...), plus BLAZDRAGCFG pour les réglages.
   Le VMU « actif » reçoit les sauvegardes : c'est celui qui contient BLAZDRAGCFG (ou, à défaut, des sauvegardes,
   ou, à défaut, le premier présent). L'écran Options > Carte mémoire > VMU permet d'en choisir un autre. */
static const char *kVmuDir[8] = { "/vmu/a1", "/vmu/a2", "/vmu/b1", "/vmu/b2", "/vmu/c1", "/vmu/c2", "/vmu/d1", "/vmu/d2" };
static int g_vmuActive = -1;
static bool g_vmuInit = false;

/* Presence : on interroge la liste des peripheriques Maple (aucun acces disque, pas de message d'erreur de KOS). */
static bool vmuPresent(int i) {
	maple_device_t *d = maple_enum_dev(kVmuDir[i][5] - 'a', kVmuDir[i][6] - '0');
	return d && (d->info.functions & MAPLE_FUNC_MEMCARD);
}

static void vmuFile(int i, const char *name, char *path) { snprintf(path, 48, "%s/%s", kVmuDir[i], name); }

static bool vmuHas(int i, const char *name) {
	char p[48]; vmuFile(i, name, p);
	file_t f = fs_open(p, O_RDONLY | O_META);
	if (f == FILEHND_INVALID) return false;
	fs_close(f);
	return true;
}

static void vmuSlotName(int slot, char *out) { snprintf(out, 16, "BLAZDRAG%03d", slot + 1); }

static void vmuChooseActive() {
	int first = -1, withSave = -1, withCfg = -1;
	for (int i = 0; i < 8; i++) {
		if (!vmuPresent(i)) continue;
		if (first < 0) first = i;
		if (withCfg < 0 && vmuHas(i, "BLAZDRAGCFG")) withCfg = i;
		if (withSave < 0) for (int s = 0; s < 3; s++) { char n[16]; vmuSlotName(s, n); if (vmuHas(i, n)) { withSave = i; break; } }
	}
	g_vmuActive = withCfg >= 0 ? withCfg : (withSave >= 0 ? withSave : first);
	g_vmuInit = true;
}

static bool vmuFindDir(char *dir) {
	/* Un VMU occupé peut disparaître un instant de la liste Maple : on attend qu'il revienne au lieu de basculer
	   en silence sur un autre VMU (les sauvegardes partaient alors sur A2 et la liste lisait tantôt A1, tantôt A2). */
	if (g_vmuInit && g_vmuActive >= 0 && !vmuPresent(g_vmuActive))
		for (int k = 0; k < 25 && !vmuPresent(g_vmuActive); k++) thd_sleep(20);
	if (!g_vmuInit || g_vmuActive < 0 || !vmuPresent(g_vmuActive)) vmuChooseActive();
	if (g_vmuActive < 0) return false;
	strcpy(dir, kVmuDir[g_vmuActive]);
	return true;
}

static bool vmuPath(const Common::String &name, char *path, bool *isCfg) {
	char dir[16];
	if (!vmuFindDir(dir)) return false;
	const char *n = name.c_str();
	const char *dot = strrchr(n, '.');
	*isCfg = (dot && strcmp(dot, ".cfg") == 0);
	if (*isCfg) snprintf(path, 48, "%s/BLAZDRAGCFG", dir);
	else snprintf(path, 48, "%s/BLAZDRAG%03d", dir, dot ? atoi(dot + 1) : 0);
	return true;
}

/* lecture / écriture brutes d'un fichier VMU (entête comprise) */
static uint8_t *vmuRawRead(const char *path, ssize_t *n) {
	file_t f = fs_open(path, O_RDONLY | O_META);
	if (f == FILEHND_INVALID) return nullptr;
	ssize_t total = fs_total(f);
	if (total <= 0 || total > 65536) { fs_close(f); return nullptr; }
	uint8_t *buf = (uint8_t *)malloc(total);
	ssize_t r = buf ? fs_read(f, buf, total) : -1;
	fs_close(f);
	if (r != total) { free(buf); return nullptr; }
	*n = total;
	return buf;
}

static bool vmuRawWrite(const char *path, const uint8_t *data, ssize_t n) {
	fs_unlink(path);
	file_t f = fs_open(path, O_WRONLY | O_META);
	if (f == FILEHND_INVALID) return false;
	ssize_t w = fs_write(f, data, n);
	fs_close(f);
	return w == n;
}

/* Les acces VMU sont lents (jusqu'a 2 s sous Flycast) : l'etat est mis en cache, rafraichi par Vmu::refresh()
   a l'ouverture des menus et apres chaque action. */
static int g_cnt = -1, g_freeCache[8];
static int g_ids[8];
static void vmuRebuild() {
	g_cnt = 0;
	for (int i = 0; i < 8; i++) { g_freeCache[i] = -2; if (vmuPresent(i)) g_ids[g_cnt++] = i; }
}
static int vmuIdOf(int idx) { if (g_cnt < 0) vmuRebuild(); return (idx >= 0 && idx < g_cnt) ? g_ids[idx] : -1; }

namespace Vmu {
void refresh() { vmuRebuild(); }
int count() { if (g_cnt < 0) vmuRebuild(); return g_cnt; }
const char *name(int idx) {
	static char n[8][4];
	int id = vmuIdOf(idx);
	if (id < 0) return "?";
	n[id][0] = (char)(kVmuDir[id][5] - 32); n[id][1] = kVmuDir[id][6]; n[id][2] = 0;
	return n[id];
}
int freeBlocks(int idx) {
	int id = vmuIdOf(idx);
	if (id < 0) return -1;
	if (g_freeCache[id] == -2) {
		maple_device_t *dev = maple_enum_dev(kVmuDir[id][5] - 'a', kVmuDir[id][6] - '0');
		g_freeCache[id] = dev ? vmufs_free_blocks(dev) : -1;
	}
	return g_freeCache[id];
}
bool slotUsed(int idx, int slot) {
	int id = vmuIdOf(idx);
	if (id < 0 || slot < 0 || slot >= kSlots) return false;
	char n[16]; vmuSlotName(slot, n);
	return vmuHas(id, n);
}
int savesUsed(int idx) { int k = 0; for (int s = 0; s < kSlots; s++) if (slotUsed(idx, s)) k++; return k; }
int active() {
	if (!g_vmuInit) vmuChooseActive();
	for (int k = 0; k < count(); k++) if (g_ids[k] == g_vmuActive) return k;
	return -1;
}
bool setActive(int idx) {
	int id = vmuIdOf(idx);
	if (id < 0) return false;
	if (id == g_vmuActive) return true;
	/* le fichier de réglages suit le VMU actif (c'est lui qui désigne le VMU au prochain démarrage) */
	char from[48], to[48];
	if (g_vmuActive >= 0 && vmuHas(g_vmuActive, "BLAZDRAGCFG")) {
		vmuFile(g_vmuActive, "BLAZDRAGCFG", from); vmuFile(id, "BLAZDRAGCFG", to);
		ssize_t n; uint8_t *b = vmuRawRead(from, &n);
		if (b) { bool ok = vmuRawWrite(to, b, n); free(b); if (!ok) return false; fs_unlink(from); }
	} else {
		vmuFile(id, "BLAZDRAGCFG", to);   /* sera créé à la prochaine sortie du menu Options */
		(void)to;
	}
	g_vmuActive = id; g_vmuInit = true;
	return true;
}
bool copySlot(int from, int to, int slot) {
	int a = vmuIdOf(from), b = vmuIdOf(to);
	if (a < 0 || b < 0 || a == b || slot < 0 || slot >= kSlots) return false;
	char n[16]; vmuSlotName(slot, n);
	char pa[48], pb[48]; vmuFile(a, n, pa); vmuFile(b, n, pb);
	ssize_t sz; uint8_t *buf = vmuRawRead(pa, &sz);
	if (!buf) return false;
	bool ok = vmuRawWrite(pb, buf, sz);
	free(buf);
	g_freeCache[b] = -2;
	return ok;
}
bool eraseSlot(int idx, int slot) {
	int id = vmuIdOf(idx);
	if (id < 0 || slot < 0 || slot >= kSlots) return false;
	char n[16], p[48]; vmuSlotName(slot, n); vmuFile(id, n, p);
	g_freeCache[id] = -2;
	return fs_unlink(p) == 0;
}
}

class DcOutSave : public Common::MemoryWriteStreamDynamic {
public:
	char path[48]; bool cfg;
	bool written = false, failed = false;
	DcOutSave(const char *p, bool c) : Common::MemoryWriteStreamDynamic(DisposeAfterUse::YES), cfg(c) { snprintf(path, sizeof(path), "%s", p); }
	/* L'écriture a lieu à finalize() pour que l'appelant sache si elle a réussi (err()) avant d'afficher « Partie sauvegardée ». */
	void finalize() override { writeNow(); }
	bool err() const override { return failed; }
	~DcOutSave() override { writeNow(); }
	void writeNow() {
		if (written) return;
		written = true;
		static uint8_t icon[512];
		for (int y = 0; y < 32; y++)
			for (int x = 0; x < 32; x += 2) {
				bool edge = (y < 2 || y >= 30), e0 = edge || x < 2, e1 = edge || x + 1 >= 30;
				icon[y * 16 + x / 2] = (uint8_t)((e0 ? 1 : 2) | ((e1 ? 1 : 2) << 4));
			}
		vmu_pkg_t pkg;
		memset(&pkg, 0, sizeof(pkg));
		strcpy(pkg.desc_short, "Blazing Dragons");
		strcpy(pkg.desc_long, Dragons::g_bdLangFr ? (cfg ? "Reglages" : "Sauvegarde") : (cfg ? "Settings" : "Saved game"));
		strcpy(pkg.app_id, "BlazingDragonsDC");
		pkg.icon_cnt = 1; pkg.icon_anim_speed = 0; pkg.eyecatch_type = VMUPKG_EC_NONE;
		pkg.icon_pal[0] = 0xF000; pkg.icon_pal[1] = 0xFFD0; pkg.icon_pal[2] = 0xFC20;
		pkg.icon_data = icon;
		pkg.data_len = (int)size(); pkg.data = getData();
		uint8_t *out = nullptr; int outSize = 0;
		if (vmu_pkg_build(&pkg, &out, &outSize) < 0 || !out) { dbglog(DBG_ERROR, "BD: VMU emballage impossible\n"); failed = true; return; }
		bool ok = vmuRawWrite(path, out, outSize);
		if (ok) {   /* relecture : vérifie que le fichier existe bien avec la bonne taille */
			ssize_t n = 0; uint8_t *chk = vmuRawRead(path, &n);
			ok = chk && n >= outSize;
			free(chk);
		}
		failed = !ok;
		dbglog(ok ? DBG_INFO : DBG_ERROR, "BD: VMU %s : %s (%d octets)\n", path, ok ? "ecrit" : "ECHEC d'ecriture", outSize);
		free(out);
	}
};

class DcSaves : public Common::SaveFileManager {
public:
	Common::InSaveFile *openForLoading(const Common::String &name) override {
		char path[48]; bool cfg;
		if (!vmuPath(name, path, &cfg)) return nullptr;
		ssize_t total; uint8_t *buf = vmuRawRead(path, &total);
		if (!buf) return nullptr;
		ssize_t r = total;
		vmu_pkg_t pkg;
		if (r != total || vmu_pkg_parse(buf, total, &pkg) < 0 || pkg.data_len <= 0 || pkg.data_len > total) { free(buf); return nullptr; }
		uint8_t *copy = (uint8_t *)malloc(pkg.data_len);
		if (!copy) { free(buf); return nullptr; }
		memcpy(copy, pkg.data, pkg.data_len);
		free(buf);
		return new Common::MemoryReadStream(copy, pkg.data_len, DisposeAfterUse::YES);
	}
	Common::OutSaveFile *openForSaving(const Common::String &name, bool) override {
		char path[48]; bool cfg;
		if (!vmuPath(name, path, &cfg)) return nullptr;
		return new DcOutSave(path, cfg);
	}
	bool removeSavefile(const Common::String &name) override {
		char path[48]; bool cfg;
		return vmuPath(name, path, &cfg) && fs_unlink(path) == 0;
	}
	Common::StringArray listSavefiles(const Common::String &) override { return Common::StringArray(); }
};

class DcSystem : public OSystem {
public:
	uint16 *conv;        /* tampon RGB565 aligné 32 octets pour l'envoi */
	pvr_ptr_t tex;
	DcEvents events;
	DcSaves saves;
	uint64 start;
	int frame = 0;
	uint32 prevButtons = 0, prevRaw = 0;
	uint32 stickDirs = 0;   /* directions du stick analogique vues comme la croix (avec hystérésis) */
	int mx = W / 2, my = H / 2;
	bool showMem = true;
#ifdef BD_GPU
	Gpu::Core *gpuCore = nullptr; DcPvrSink *gpuSink = nullptr;
#endif
	int dirtyY0 = 0, dirtyY1 = H;   /* lignes à reconvertir / renvoyer [y0, y1) */
	int dpadHold = 0;               /* images où la croix est maintenue (accélération du curseur) */
	uint64 tDraw = 0, tStart = 0;   /* mesures : temps passé à dessiner, début de la période */

	DcSystem() {
		dbglog(DBG_INFO, "BD: DcSystem debut\n");
#ifdef BD_GPU
		/* Liste opaque (première couche du décor) puis liste translucide sans tri automatique : ordre d'envoi = ordre de dessin. */
		pvr_init_params_t params;
		memset(&params, 0, sizeof(params));
		params.opb_sizes[0] = PVR_BINSIZE_16;   /* liste opaque : première couche du décor */
		params.opb_sizes[1] = PVR_BINSIZE_0;
		params.opb_sizes[2] = PVR_BINSIZE_32;
		params.opb_sizes[3] = PVR_BINSIZE_0;
		params.opb_sizes[4] = PVR_BINSIZE_0;
		params.vertex_buf_size = 256 * 1024;
		params.autosort_disabled = 1;
		params.opb_overflow_count = 3;
		pvr_init(&params);
#else
		pvr_init_defaults();
#endif
		dbglog(DBG_INFO, "BD: pvr_init ok (VRAM libre %lu Ko)\n", (unsigned long)(pvr_mem_available() / 1024));
#ifdef BD_GPU
		conv = nullptr; tex = nullptr;   /* la composition se fait sur le PVR */
#else
		conv = (uint16 *)memalign(32, TW * H * 2);
		memset(conv, 0, TW * H * 2);
		tex = pvr_mem_malloc(TW * TH * 2);
#endif
		start = timer_ms_gettime64();
		BdProf::nowFn = []() -> unsigned long long { return (unsigned long long)timer_ms_gettime64(); };
		dbglog(DBG_INFO, "BD: DcSystem ok (conv=%p tex=%p)\n", (void *)conv, (void *)tex);
	}

	uint32 getMillis(bool) override { return (uint32)(timer_ms_gettime64() - start); }
	void delayMillis(uint ms) override { if (ms) thd_sleep(ms); else thd_pass(); }

	/* Format du jeu : R bits 10-14, G bits 5-9, B bits 0-4 (RGB555, comme PixelFormat de screen.cpp).
	 * PVR : RGB565. Le bit faible du vert est une copie du bit fort. */
	static inline uint16 cv1(uint32 c) {
		return ((c & 0x7c00) << 1) | ((c & 0x03e0) << 1) | (c & 0x1f) | ((c & 0x200) >> 4);
	}
	static inline uint32 cv2(uint32 c) {   /* deux pixels à la fois */
		return ((c & 0x7c007c00) << 1) | ((c & 0x03e003e0) << 1) | (c & 0x001f001f) | ((c & 0x02000200) >> 4);
	}

	/* Convertit directement dans le tampon d'envoi (une seule passe, pas de copie intermédiaire). */
	void copyRectToScreen(const void *buf, int pitch, int x, int y, int w, int h) override {
		if (!conv) return;
		if (x < 0) { buf = (const byte *)buf - x * 2; w += x; x = 0; }
		if (y < 0) { buf = (const byte *)buf - y * pitch; h += y; y = 0; }
		if (x + w > W) w = W - x;
		if (y + h > H) h = H - y;
		if (w <= 0 || h <= 0) return;
		if (y < dirtyY0) dirtyY0 = y;
		if (y + h > dirtyY1) dirtyY1 = y + h;
		for (int j = 0; j < h; j++) {
			const uint16 *sp = (const uint16 *)((const byte *)buf + j * pitch);
			uint16 *dp = conv + (y + j) * TW + x;
			int i = 0;
			if ((((uintptr_t)sp | (uintptr_t)dp) & 3) == 0) {
				for (; i + 1 < w; i += 2) *(uint32 *)(dp + i) = cv2(*(const uint32 *)(sp + i));
			}
			for (; i < w; i++) dp[i] = cv1(sp[i]);
		}
	}
	void fillScreen(uint32 col) override {
		if (!conv) return;
		uint16 v = cv1(col);
		for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) conv[y * TW + x] = v;
		dirtyY0 = 0; dirtyY1 = H;
	}

	void draw() {
		if (!conv) return;
		uint64 t0 = timer_ms_gettime64();
		int r0 = dirtyY0, r1 = dirtyY1;
		if (r1 > r0) {
			dcache_flush_range((uintptr_t)(conv + r0 * TW), (r1 - r0) * TW * 2);
			pvr_txr_load(conv + r0 * TW, (pvr_ptr_t)((uintptr_t)tex + r0 * TW * 2), (r1 - r0) * TW * 2);
		}
		dirtyY0 = H; dirtyY1 = 0;
		tDraw += timer_ms_gettime64() - t0;

		pvr_wait_ready();
		pvr_scene_begin();
		pvr_list_begin(PVR_LIST_OP_POLY);

		pvr_poly_cxt_t cxt;
		pvr_poly_hdr_t hdr;
		pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
		                 TW, TH, tex, PVR_FILTER_NONE);
		pvr_poly_compile(&hdr, &cxt);
		pvr_prim(&hdr, sizeof(hdr));

		const float x0 = 0, y0 = 40, x1 = 640, y1 = 440;
		const float u1 = (float)W / TW, v1 = (float)H / TH;
		pvr_vertex_t v;
		v.argb = 0xffffffff; v.oargb = 0; v.z = 1;
		v.flags = PVR_CMD_VERTEX;
		v.x = x0; v.y = y1; v.u = 0;  v.v = v1; pvr_prim(&v, sizeof(v));
		v.x = x0; v.y = y0; v.u = 0;  v.v = 0;  pvr_prim(&v, sizeof(v));
		v.x = x1; v.y = y1; v.u = u1; v.v = v1; pvr_prim(&v, sizeof(v));
		v.flags = PVR_CMD_VERTEX_EOL;
		v.x = x1; v.y = y0; v.u = u1; v.v = 0;  pvr_prim(&v, sizeof(v));

		pvr_list_finish();
		pvr_scene_finish();
	}

	void action(int act, bool down) {
		Common::Event e;
		e.type = down ? Common::EVENT_CUSTOM_ENGINE_ACTION_START : Common::EVENT_CUSTOM_ENGINE_ACTION_END;
		e.customType = act;
		events.queue.push_back(e);
	}

	void moveMouse(int dx, int dy) {
		if (!dx && !dy) return;
		mx += dx; my += dy;
		if (mx < 0) mx = 0;
		if (mx >= W) mx = W - 1;
		if (my < 0) my = 0;
		if (my >= H) my = H - 1;
		Common::Event e;
		e.type = Common::EVENT_MOUSEMOVE;
		e.mouse.x = mx; e.mouse.y = my;
		events.queue.push_back(e);
	}

	static int axisSpeed(int a) {
		if (a > -12 && a < 12) return 0;
		int v = (a * (a < 0 ? -a : a)) / 2048;
		return v ? v : (a > 0 ? 1 : -1);
	}

	void pollInput() {
		uint32 buttons = 0;
		maple_device_t *cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
		if (cont) {
			cont_state_t *st = (cont_state_t *)maple_dev_status(cont);
			if (st) {
				buttons = st->buttons;
				if (st->ltrig > 128) buttons |= 1u << 30;
				if (st->rtrig > 128) buttons |= 1u << 31;
				int ax = st->joyx, ay = st->joyy;
				if (ax > 16 || ax < -16 || ay > 16 || ay < -16)
					moveMouse(ax / 24, ay / 24);
				/* Stick franchement poussé = croix directionnelle pour les actions (mini-jeux comme la chatapulte, menus).
				   Hystérésis : enclenché au-delà de 64, relâché sous 40, pour éviter les rebonds. */
				auto dir = [&](int v, int neg, int pos) {
					const bool wasN = stickDirs & neg, wasP = stickDirs & pos;
					stickDirs &= ~(neg | pos);
					if (v < -64 || (wasN && v < -40)) stickDirs |= neg;
					else if (v > 64 || (wasP && v > 40)) stickDirs |= pos;
				};
				dir(ax, CONT_DPAD_LEFT, CONT_DPAD_RIGHT);
				dir(ay, CONT_DPAD_UP, CONT_DPAD_DOWN);
			}
		}
		maple_device_t *mouse = maple_enum_type(0, MAPLE_FUNC_MOUSE);
		if (mouse) {
			mouse_state_t *ms = (mouse_state_t *)maple_dev_status(mouse);
			if (ms) {
				moveMouse(ms->dx / 2, ms->dy / 2);
				if (ms->buttons & MOUSE_LEFTBUTTON) buttons |= CONT_A;
				if (ms->buttons & MOUSE_RIGHTBUTTON) buttons |= CONT_B;
			}
		}
		/* Boutons logiques (indépendants de KOS) : fonctions réattribuables dans Options > Contrôles. */
		uint32 raw = 0;
		if (buttons & CONT_A) raw |= InputMap::BTN_A;
		if (buttons & CONT_B) raw |= InputMap::BTN_B;
		if (buttons & CONT_X) raw |= InputMap::BTN_X;
		if (buttons & CONT_Y) raw |= InputMap::BTN_Y;
		if (buttons & (1u << 30)) raw |= InputMap::BTN_L;
		if (buttons & (1u << 31)) raw |= InputMap::BTN_R;
		InputMap::pressedNow |= raw & ~prevRaw;
		struct { uint32 mask; int act; } map[12] = {};
		int nMap = 0;
		map[nMap++] = { CONT_DPAD_UP, Dragons::kDragonsActionUp };
		map[nMap++] = { CONT_DPAD_DOWN, Dragons::kDragonsActionDown };
		map[nMap++] = { CONT_DPAD_LEFT, Dragons::kDragonsActionLeft };
		map[nMap++] = { CONT_DPAD_RIGHT, Dragons::kDragonsActionRight };
		map[nMap++] = { CONT_START, Dragons::kDragonsActionMenu };
		if (Dragons::g_bdPadPositional) {   /* concours de danse : A croix, B rond, X carré, Y triangle, L L1, R R1 */
			map[nMap++] = { CONT_A, Dragons::kDragonsActionCross };
			map[nMap++] = { CONT_B, Dragons::kDragonsActionCircle };
			map[nMap++] = { CONT_X, Dragons::kDragonsActionSquare };
			map[nMap++] = { CONT_Y, Dragons::kDragonsActionTriangle };
			map[nMap++] = { 1u << 30, Dragons::kDragonsActionL1 };
			map[nMap++] = { 1u << 31, Dragons::kDragonsActionR1 };
		} else {
		static const int fnAct[InputMap::F_COUNT] = { Dragons::kDragonsActionSelect, Dragons::kDragonsActionInventory, Dragons::kDragonsActionR1, Dragons::kDragonsActionL1 };
		uint32 used = 0;
		for (int f = 0; f < InputMap::F_COUNT; f++) used |= InputMap::assign[f];
		/* boutons logiques -> masques KOS pour les fonctions */
		static const struct { uint32 bit; uint32 kos; } kosOf[] = { { InputMap::BTN_A, CONT_A }, { InputMap::BTN_B, CONT_B }, { InputMap::BTN_X, CONT_X },
			{ InputMap::BTN_Y, CONT_Y }, { InputMap::BTN_L, 1u << 30 }, { InputMap::BTN_R, 1u << 31 } };
		for (int f = 0; f < InputMap::F_COUNT; f++)
			for (auto &k : kosOf) if (k.bit == InputMap::assign[f]) map[nMap++] = { k.kos, fnAct[f] };
		/* B (changer de commande) et X (croix) gardent leur rôle d'origine tant qu'aucune fonction ne les utilise */
		if (!(used & InputMap::BTN_B)) map[nMap++] = { CONT_B, Dragons::kDragonsActionChangeCommand };
		if (!(used & InputMap::BTN_X)) map[nMap++] = { CONT_X, Dragons::kDragonsActionCross };
		}
		/* Croix directionnelle : déplace aussi le curseur (comme sur PlayStation). */
		int dx = 0, dy = 0;
		if (buttons & CONT_DPAD_LEFT) dx -= 1;
		if (buttons & CONT_DPAD_RIGHT) dx += 1;
		if (buttons & CONT_DPAD_UP) dy -= 1;
		if (buttons & CONT_DPAD_DOWN) dy += 1;
		Dragons::DragonsEngine *eng = Dragons::getEngine();
		if (eng && eng->isInMenu()) { dx = dy = 0; }   /* dans les menus, la croix ne deplace pas le curseur du jeu */
		if (dx || dy) {
			int sp = 2 + dpadHold / 15; if (sp > 6) sp = 6;
			moveMouse(dx * sp, dy * sp);
			dpadHold++;
		} else dpadHold = 0;
		const uint32 actButtons = buttons | (cont ? stickDirs : 0);   /* le stick ne déplace pas deux fois le curseur : seulement les actions */
		uint32 changed = actButtons ^ prevButtons;
		for (int i = 0; i < nMap; i++)
			if (changed & map[i].mask) action(map[i].act, (actButtons & map[i].mask) != 0);
		prevButtons = actButtons;
		prevRaw = raw;
	}

	uint64 lastFrameT = 0, slowDisc = 0, slowPrep = 0, slowTex = 0, slowWait = 0;
	void updateScreen() override {
		frame++;
		if (frame <= 3) dbglog(DBG_INFO, "BD: updateScreen %d\n", frame);
		draw();
		pollInput();
		/* Mesures à l'écran (option « Jauges », ou build « perf »), en haut à gauche (1 pixel du jeu = 1 ms ; traits blancs à 17 ms et 33 ms) :
		   ligne 1 : durée de l'image précédente (vert <= 17 ms, jaune <= 34 ms, rouge au-delà)
		   ligne 2 : répartition : bleu disque, orange préparation GPU (dont violet création de textures),
		             gris attente du PVR, blanc reste (logique du jeu, son, entrées)
		   ligne 3 : pire image des 2 dernières secondes */
		if (gpuSink && !Dragons::g_bdPerfHud) gpuSink->nHud = 0;
		if (gpuSink && Dragons::g_bdPerfHud) {
			static uint64 hLast, hDisc, hPrep, hTex, hWait; static uint32 hist[120]; static int hi;
			const uint64 now = timer_ms_gettime64();
			const uint32 tot = hLast ? (uint32)(now - hLast) : 0;
			const uint32 dd = (uint32)(BdProf::ms[BdProf::DISC] - hDisc), dp = (uint32)(BdProf::ms[BdProf::PREP] - hPrep);
			const uint32 dt = (uint32)(BdProf::ms[BdProf::TEX] - hTex), dw = (uint32)(BdProf::ms[BdProf::WAIT] - hWait);
			hLast = now; hDisc = BdProf::ms[BdProf::DISC]; hPrep = BdProf::ms[BdProf::PREP]; hTex = BdProf::ms[BdProf::TEX]; hWait = BdProf::ms[BdProf::WAIT];
			hist[hi = (hi + 1) % 120] = tot;
			uint32 worst = 0; for (uint32 v : hist) worst = MAX(worst, v);
			DcPvrSink &k = *gpuSink; k.nHud = 0;
			auto bar = [&](float x0, float y, float len, uint16 c) {
				if (len <= 0 || k.nHud >= 24) return;
				k.hud[k.nHud++] = { x0, y, MIN(x0 + len, 316.0f), y + 3, c };
			};
			const float X = 4;
			bar(X - 1, 1, 302, 0x0000);                       /* fond noir */
			bar(X - 1, 5, 302, 0x0000);
			bar(X - 1, 9, 302, 0x0000);
			bar(X, 1, (float)tot, tot <= 17 ? 0x03e0 : (tot <= 34 ? 0x7fe0 : 0x7c00));
			float x = X;
			bar(x, 5, (float)dd, 0x001f); x += dd;
			bar(x, 5, (float)(dp > dt ? dp - dt : 0), 0x7e00); x += (dp > dt ? dp - dt : 0);
			bar(x, 5, (float)dt, 0x5c17); x += dt;
			bar(x, 5, (float)dw, 0x4210); x += dw;
			const uint32 used = dd + dp + dw;
			bar(x, 5, (float)(tot > used ? tot - used : 0), 0x7fff);
			bar(X, 9, (float)worst, 0x7c00);
			bar(X + 17, 0, 1, 0x7fff); bar(X + 33, 0, 1, 0x7fff);
			k.hud[k.nHud - 1].y1 = 12; k.hud[k.nHud - 2].y1 = 12;
		}
#ifdef BD_LOG   /* statistiques periodiques : desactivees par defaut, la sortie serie bloque le jeu (~100 ms a chaque bloc) */
		if (frame % 300 == 0) {
			uint64 now = timer_ms_gettime64();
			dbglog(DBG_INFO, "image %d : %lu ms / 300 images (%.1f i/s), dessin %lu ms\n", frame,
			       (unsigned long)(now - tStart), 300000.0 / (double)(now - tStart + 1), (unsigned long)tDraw);
			tStart = now; tDraw = 0;
			dbglog(DBG_INFO, "  temps : disque %llu ms (%u acces, %llu Ko), prep GPU %llu ms (dont creation textures %llu ms, %u), present %llu ms, attente PVR %llu ms\n",
			       BdProf::ms[BdProf::DISC], BdProf::n[BdProf::DISC], BdProf::discBytes / 1024, BdProf::ms[BdProf::PREP],
			       BdProf::ms[BdProf::TEX], BdProf::n[BdProf::TEX], BdProf::ms[BdProf::PRESENT], BdProf::ms[BdProf::WAIT]);
			dbglog(DBG_INFO, "  detail prep (ms/appels) : sprites %llu/%u, images16 %llu/%u, video %llu/%u, rect %llu/%u, couches %llu/%u, couchesHD %llu/%u, spritesHD %llu/%u, defilY %llu/%u\n",
			       BdProf::subMs[0], BdProf::subN[0], BdProf::subMs[1], BdProf::subN[1], BdProf::subMs[2], BdProf::subN[2], BdProf::subMs[3], BdProf::subN[3],
			       BdProf::subMs[4], BdProf::subN[4], BdProf::subMs[5], BdProf::subN[5], BdProf::subMs[6], BdProf::subN[6], BdProf::subMs[7], BdProf::subN[7]);
			for (int k = 0; k < BdProf::S_COUNT; k++) { BdProf::subMs[k] = 0; BdProf::subN[k] = 0; }
			{
				unsigned long ms, fr; int vc;
				dcAudioStats(&ms, &fr, &vc);
				unsigned clip = Snd::Mixer::get().statClip; Snd::Mixer::get().statClip = 0;
				dbglog(DBG_INFO, "  son : melange %lu ms pour %lu trames (%.1f%% du temps reel), %d voix, %u saturations, intervalle max %lu ms\n", ms, fr,
				       fr ? 100.0 * ms / (fr * 1000.0 / Snd::OUT_RATE) : 0.0, vc, clip, dcAudioMaxGap());
				{ unsigned up, fl, kb; dcAudioHwStats(&up, &fl, &kb); if (up || fl) dbglog(DBG_INFO, "  son materiel : %u echantillons dans la RAM AICA (%u Ko), %u refuses (restent en logiciel)\n", up, kb, fl); }
			}
			dbglog(DBG_INFO, "  lecture : %llu commandes disque, %llu Ko transferes ; prechargement : %u lues en avance (%llu Ko), %u utilisees, %u non pretes, %u Ko en memoire\n",
			       Disc::statCommands, Disc::statBytes / 1024, Prefetch::statLoaded, Prefetch::statBytesLoaded / 1024,
			       Prefetch::statHit, Prefetch::statMiss, (unsigned)(Prefetch::storedBytes() / 1024));
			for (int i = 0; i < BdProf::COUNT; i++) { BdProf::ms[i] = 0; BdProf::n[i] = 0; }
			BdProf::discBytes = 0;
		}
		{	/* image anormalement longue : on note ce qui l'a occupee */
			uint64 now = timer_ms_gettime64();
			if (lastFrameT && now - lastFrameT > 150)
				dbglog(DBG_INFO, "  LENT image %d : %lu ms (disque %llu, prep %llu, textures %llu, attente %llu)\n", frame,
				       (unsigned long)(now - lastFrameT), BdProf::ms[BdProf::DISC] - slowDisc, BdProf::ms[BdProf::PREP] - slowPrep,
				       BdProf::ms[BdProf::TEX] - slowTex, BdProf::ms[BdProf::WAIT] - slowWait);
			lastFrameT = now;
			slowDisc = BdProf::ms[BdProf::DISC]; slowPrep = BdProf::ms[BdProf::PREP];
			slowTex = BdProf::ms[BdProf::TEX]; slowWait = BdProf::ms[BdProf::WAIT];
		}
		if (showMem && frame % 300 == 0) {
			struct mallinfo mi = mallinfo();
			dbglog(DBG_INFO, "image %d : tas utilisé %d Ko, libre %d Ko, VRAM libre %lu Ko\n",
			       frame, mi.uordblks / 1024, mi.fordblks / 1024,
			       (unsigned long)(pvr_mem_available() / 1024));
#ifdef BD_GPU
			if (gpuCore)
				dbglog(DBG_INFO, "GPU : %u textures creees, %u evincees, %u echecs, %u sautees, %u decors HD a recharger, %u quads, VRAM textures %lu Ko\n",
				       gpuCore->stats.texCreated, gpuCore->stats.texEvicted, gpuCore->stats.texFailed, gpuCore->stats.skipped, gpuCore->stats.hdLoadRetry,
				       gpuCore->stats.quads, (unsigned long)(gpuCore->usedBytes() / 1024));
#endif
			if (gpuCore) {
				dbglog(DBG_INFO, "HD sprites : %u dessines, %u trames absentes du pack, %u palette autre ; textures echouees %u ; index %u Ko en memoire, %u chargements\n",
				       gpuCore->stats.hdSprHit, gpuCore->stats.hdSprMissNoFrame, (unsigned)Dragons::g_hdSprPalMiss, gpuCore->stats.texFailed,
				       g_hdSprPackPtr ? (unsigned)(g_hdSprPackPtr->indexBytes() / 1024) : 0u, g_hdSprPackPtr ? (unsigned)g_hdSprPackPtr->indexLoads() : 0u);
				dbglog(DBG_INFO, "HD decors : %u essais, %u couche modifiee, %u palette modifiee, %u dessines, %u echecs (dernier %s, palette %d diff)\n",
				       (unsigned)Dragons::g_hdLayTry, (unsigned)Dragons::g_hdLayNotPristine, (unsigned)Dragons::g_hdLayPal, (unsigned)Dragons::g_hdLayHit,
				       (unsigned)Dragons::g_hdLayFail, Dragons::g_hdLayFailName, Dragons::g_hdPalDiff[0]);
				dbglog(DBG_INFO, "  derniere palette differente : entree %d, courante %04x, decor %04x\n", Dragons::g_hdPalDiff[1], Dragons::g_hdPalDiff[2], Dragons::g_hdPalDiff[3]);
				/* seulement les couches dont les compteurs ont bougé depuis le dernier bloc : la sortie série bloque le jeu
				   (~5 ms par ligne), la table complète finissait par figer l'image ~0,3 s toutes les 5 s */
				static uint32 lastSum[32];
				for (int i = 0; i < Dragons::g_hdLayStatN; i++) {
					const uint32 sum = Dragons::g_hdLayStat[i].hit + Dragons::g_hdLayStat[i].fail + Dragons::g_hdLayStat[i].pal + Dragons::g_hdLayStat[i].notp;
					if (sum == lastSum[i]) continue;
					lastSum[i] = sum;
					dbglog(DBG_INFO, "  decor %s couche %d (mode %d) : %u HD, %u original, %u palette modifiee, %u couche modifiee\n", Dragons::g_hdLayStat[i].name, Dragons::g_hdLayStat[i].layer, Dragons::g_hdLayStat[i].mode,
					       Dragons::g_hdLayStat[i].hit, Dragons::g_hdLayStat[i].fail, Dragons::g_hdLayStat[i].pal, Dragons::g_hdLayStat[i].notp);
				}
			}
		}
#endif /* BD_LOG */
	}

	Common::EventManager *getEventManager() override { return &events; }
	Common::SaveFileManager *getSavefileManager() override { return &saves; }
};

/* ---- Préchargement en arrière-plan (voir port/common/prefetch.h) ---- */
static mutex_t g_pfMutex = MUTEX_INITIALIZER;
static void pfLock() { mutex_lock(&g_pfMutex); }
static void pfUnlock() { mutex_unlock(&g_pfMutex); }
/* mémoire encore utilisable : trous du tas + ce que sbrk peut encore obtenir (marge pour la pile) */
static unsigned pfFreeKB() {
	struct mallinfo mi = mallinfo();
	long top = (long)_arch_mem_top - 256 * 1024;
	long rest = top - (long)sbrk(0);
	if (rest < 0) rest = 0;
	return (unsigned)(((long)mi.fordblks + rest) / 1024);
}
static void pfSleep(int ms) { thd_sleep(ms); }
static void *pfThread(void *) {
	for (;;) {
		if (Prefetch::pump()) thd_pass();
		else thd_sleep(150);
	}
	return nullptr;
}
static void startPrefetch() {
	Prefetch::Env e = { pfFreeKB, pfLock, pfUnlock };
	Prefetch::setEnv(e);
	Disc::setSleepHook(pfSleep);
	if (thd_create(true, pfThread, nullptr)) dbglog(DBG_INFO, "BD: prechargement en arriere-plan actif\n");
	else dbglog(DBG_ERROR, "BD: thread de prechargement impossible\n");
}

static void listDir(const char *path) {
	DIR *d = opendir(path);
	if (!d) { dbglog(DBG_INFO, "BD: opendir(%s) echoue\n", path); return; }
	struct dirent *e;
	while ((e = readdir(d))) dbglog(DBG_INFO, "BD:   %s/%s\n", path, e->d_name);
	closedir(d);
}


int main(int, char **) {
#ifdef BD_NOLOG
	/* aucune sortie texte : chaque printf/dbglog part sur la liaison série et peut bloquer le jeu sur vraie console */
	dbglog_set_level(DBG_DEAD);
	dbgio_disable();
#endif
	dbglog(DBG_INFO, "BD: version %s\n", BD_BUILD);
	dbglog(DBG_INFO, "BD: main demarre (RAM libre %lu Ko)\n", (unsigned long)(mallinfo().arena / 1024));
	listDir("/cd");
	/* CDI (mkdcdisc -d) : les fichiers sont dans /cd/disc ; ISO : à la racine ; /pc : dcload. */
	static const char *const dirs[] = { "/cd", "/cd/disc", "/pc" };
	const char *dataDir = "/pc";
	for (const char *d : dirs) {
		char path[64];
		snprintf(path, sizeof(path), "%s/BIGPACK.DAT", d);
		FILE *f = fopen(path, "rb");
		if (!f) { snprintf(path, sizeof(path), "%s/BIGFILE.DAT", d); f = fopen(path, "rb"); }
		if (f) { fclose(f); dataDir = d; break; }
	}
	Common::setGameDataPath(dataDir);
	dbglog(DBG_INFO, "BD: donnees dans %s\n", dataDir);
	DcSystem *sys = new DcSystem();
	g_system = sys;
	/* Disque bilingue : version anglaise dans <données>/US (fichiers communs dans <données>). Langue voulue : réglages (v6). */
	static char usDir[256]; snprintf(usDir, sizeof(usDir), "%s/US", dataDir);
	{
		char p[300]; snprintf(p, sizeof(p), "%s/DRAGON.EXE", usDir);
		FILE *f = fopen(p, "rb");
		Dragons::g_bdLangBoth = f != nullptr;
		if (f) fclose(f);
	}
	int wanted = 0;
	if (Dragons::g_bdLangBoth) {
		Common::InSaveFile *in = sys->getSavefileManager()->openForLoading("dragons.cfg");
		if (in) {
			byte b[64]; int n = in->read(b, sizeof(b));
			if (n >= 7 && b[0] == 0x42 && b[1] == 0x44 && b[2] == 0x43 && b[3] == 0x46 && b[4] >= 6) wanted = b[b[4] >= 7 ? n - 2 : n - 1] == 1 ? 1 : 0;   /* v7 : langue avant l'octet des jauges */
			delete in;
		}
		if (wanted == 1) { Common::setGameDataPath(usDir); Common::setGameDataFallback(dataDir); dataDir = usDir; }
		dbglog(DBG_INFO, "BD: disque bilingue, langue choisie %s\n", wanted ? "anglais" : "francais");
	}
	/* Changement de langue : relancer le programme. arch_reboot repasse par le BIOS (logo Dreamcast), ce qui ne marche pas
	   avec un chargeur (DreamShell) ; on relit donc le programme non brouillé (BD.BIN, à côté des données) et on saute dessus.
	   Repli : redémarrage complet. */
	static char baseDir[64]; snprintf(baseDir, sizeof(baseDir), "%s", dataDir);
	Dragons::g_bdRestart = []() {
		char p[96]; snprintf(p, sizeof(p), "%s/BD.BIN", baseDir);
		file_t f = fs_open(p, O_RDONLY);
		if (f != FILEHND_INVALID) {
			const size_t n = fs_total(f);
			void *buf = n ? memalign(32, n) : nullptr;
			const bool ok = buf && fs_read(f, buf, n) == (ssize_t)n;
			fs_close(f);
			if (ok) {
				dbglog(DBG_INFO, "BD: relance de %s (%u octets)\n", p, (unsigned)n);
				spu_disable();
				arch_exec(buf, (uint32_t)n);
			}
			free(buf);
		}
		dbglog(DBG_INFO, "BD: %s introuvable, redemarrage complet\n", p);
		arch_reboot();
	};
	dcAudioStart();
	startPrefetch();
#ifdef BD_GPU
	{
		/* Budget de textures : la VRAM libre moins une marge pour les libérations différées et les listes. */
		size_t avail = pvr_mem_available();
		size_t budget = avail > 1024 * 1024 ? avail - 1024 * 1024 : avail / 2;
		if (budget > 4 * 1024 * 1024) budget = 4 * 1024 * 1024;
		static DcPvrSink sink;
		static Gpu::Core core(&sink, budget);
		sys->gpuCore = &core; sys->gpuSink = &sink;
		/* Graphismes HD : HD.DAT (décors) et HDSPR.DAT (sprites) sur le disque ; absents = jeu d'origine seul */
		static Gpu::HdPack hdPack; static Gpu::HdSprPack hdSprPack;
		if (hdPack.open()) core.setHdPack(&hdPack);
		/* HDSPR.DAT et l'intro HD sont faits d'après la version France : avec un autre disque, sprites d'origine */
		if (hdSprPack.open()) { core.setHdSprPack(&hdSprPack); g_hdSprPackPtr = &hdSprPack; }
		dbglog(DBG_INFO, "BD: HD decors %d couches, sprites %d trames\n", hdPack.count(), hdSprPack.frameCount());
		Dragons::g_renderBackend = &core;
		dbglog(DBG_INFO, "BD: composition GPU (budget textures %lu Ko)\n", (unsigned long)(budget / 1024));
	}
#endif

	static Dragons::DragonsGameDescription desc;
	desc.desc.gameId = "dragons";
	desc.desc.language = bdDetectLanguage(dataDir);
	Dragons::g_bdLangFr = desc.desc.language == Common::FR_FRA;
	Dragons::g_bdLangWanted = Dragons::g_bdLangFr ? 0 : 1;
	Snd::Mixer::speechBoost = Dragons::g_bdLangFr ? 4096 : 5734;   /* +3 dB : répliques anglaises enregistrées plus bas (mesure : 7450 / 5260 en efficace) */
	dbglog(DBG_INFO, "BD: langue %s\n", Dragons::g_bdLangFr ? "francais" : "anglais (USA)");
	desc.desc.platform = Common::kPlatformPSX;
	desc.gameId = Dragons::kGameIdDragons;

	Dragons::DragonsEngine *engine = new Dragons::DragonsEngine(sys, &desc);
	dbglog(DBG_INFO, "BD: moteur construit, run()\n");
	engine->run();
	dbglog(DBG_INFO, "BD: run() termine\n");
	/* Fin du jeu (après le générique) : revenir de main() lançait les destructeurs et la sortie de KOS, qui plantaient
	   (erreur SH4 dans Flycast). On relance le jeu comme un retour à l'écran titre (même relance que le changement de
	   langue, qui marche aussi avec DreamShell). */
	if (Dragons::g_bdRestart) Dragons::g_bdRestart();
	arch_reboot();
	return 0;
}
