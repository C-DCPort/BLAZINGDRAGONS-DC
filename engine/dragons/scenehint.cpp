#include "dragons/scenehint.h"
#include "dragons/dragons.h"
#include "dragons/bigfile.h"
#include "dragons/dragonini.h"
#include "dragons/dragonobd.h"
#include "dragons/dragonrms.h"
#include "dragons/actorfiles.h"
#include "dragons/scene.h"
#include "prefetch.h"
#include "bdprof.h"

namespace Dragons {

SceneHint::SceneHint(DragonsEngine *vm) : _vm(vm), _prev(0) {
	for (int i = 0; i < 256; i++) _lastNext[i] = 0;
#ifndef __DREAMCAST__
	if (BdProf::envFlag("BD_SCENEDUMP")) dumpAll();
#endif
}

bool SceneHint::validScene(uint32 sceneId) const {
	sceneId &= 0x7fff;
	return sceneId > 1 && sceneId - 2 < _vm->_dragonRMS->count();
}

/* Les scripts d'objet (dragon.obd, un par objet de dragon.ini) et ceux de la scène contiennent des
   instructions « charger la scène » : mot 0x0012 suivi du numéro de scène. On les repère par balayage. */
static void scanScript(const byte *blob, const byte *dataEnd, uint32 maxScene, uint16 *out, int &n, int cap) {
	if (!blob || blob + 4 > dataEnd) return;
	uint32 len = READ_LE_UINT32(blob);
	if (len > 0x20000 || blob + 4 + len > dataEnd) return;
	const byte *c = blob + 4;
	for (uint32 i = 0; i + 8 <= len; i += 2) {
		if ((READ_LE_UINT16(c + i) & 0x7fff) != 0x12) continue;
		uint32 sc = READ_LE_UINT16(c + i + 2) & 0x7fff;
		if (sc < 2 || sc - 2 >= maxScene) continue;
		bool dup = false;
		for (int k = 0; k < n; k++) if (out[k] == sc) dup = true;
		if (!dup && n < cap) out[n++] = (uint16)sc;
	}
}

int SceneHint::successors(uint16 sceneId, uint16 *out, int cap) {
	int n = 0;
	DragonOBD *obd = _vm->_dragonOBD;
	DragonRMS *rms = _vm->_dragonRMS;
	const byte *base = obd->getObdAtOffset(0);
	const byte *end = base + obd->size();
	uint32 maxScene = rms->count();
	scanScript(rms->getBeforeSceneDataLoadedScript(sceneId), end, maxScene, out, n, cap);
	scanScript(rms->getAfterSceneDataLoadedScript(sceneId), end, maxScene, out, n, cap);
	scanScript(rms->getAfterSceneLoadedScript(sceneId), end, maxScene, out, n, cap);
	DragonINIResource *ini = _vm->_dragonINIResource;
	for (int i = 0; i < ini->totalRecords(); i++) {
		DragonINI *r = ini->getRecord(i);
		if (r->sceneId != sceneId) continue;
		scanScript(obd->tryFromOpt((uint32)r->id - 1), end, maxScene, out, n, cap);
	}
	/* on ne revient pas sur place */
	int m = 0;
	for (int i = 0; i < n; i++) if (out[i] != sceneId) out[m++] = out[i];
	return m;
}

int SceneHint::sceneFiles(uint16 sceneId, uint32 *ids, int cap) {
	BigfileArchive *bf = _vm->getBigfileArchive();
	int n = 0;
	char name[] = "XXXX.MSF";
	BdProf::copy(name, _vm->_dragonRMS->getSceneName(sceneId), 4);
	uint32 id = bf->getResourceId(name);
	if (id < bf->totalRecords() && n < cap) ids[n++] = id;
	BdProf::copy(name + 5, "SCR", 3);
	id = bf->getResourceId(name);
	if (id < bf->totalRecords() && n < cap) ids[n++] = id;
	DragonINIResource *ini = _vm->_dragonINIResource;
	for (int i = 0; i < ini->totalRecords(); i++) {
		DragonINI *r = ini->getRecord(i);
		if (r->sceneId != sceneId || !(r->flags & 1)) continue;
		if (r->actorResourceId < 0 || r->actorResourceId >= DRAGONS_NUM_ACTOR_FILES) continue;
		id = bf->getResourceId(actorResourceFiles[r->actorResourceId]);
		if (id >= bf->totalRecords()) continue;
		bool dup = false;
		for (int k = 0; k < n; k++) if (ids[k] == id) dup = true;
		if (!dup && n < cap) ids[n++] = id;
	}
	return n;
}

void SceneHint::onSceneLoaded(uint16 sceneId) {
	sceneId &= 0x7fff;
	if (!validScene(sceneId)) return;
	BigfileArchive *bf = _vm->getBigfileArchive();
	if (!bf->isPacked()) return;
	if (sceneId < 256) {
		if (_prev && _prev < 256 && _prev != sceneId) _lastNext[_prev] = sceneId;
	}
	_prev = sceneId;

	uint16 succ[MAX_SUCC];
	int ns = successors(sceneId, succ, MAX_SUCC);
	/* ordre : la scène suivante déjà vue en premier, puis les moins coûteuses (le plus de scènes prêtes) */
	uint32 files[MAX_SUCC][MAX_FILES]; int nf[MAX_SUCC]; uint32 cost[MAX_SUCC];
	int order[MAX_SUCC]; int no = 0;
	for (int i = 0; i < ns; i++) {
		if (!validScene(succ[i])) { nf[i] = 0; cost[i] = 0xffffffffu; continue; }
		nf[i] = sceneFiles(succ[i], files[i], MAX_FILES);
		cost[i] = 0;
		for (int k = 0; k < nf[i]; k++) if (!BigfileArchive::isCached(files[i][k])) cost[i] += bf->diskSize(files[i][k]);
		if (cost[i] != 0) order[no++] = i;
	}
	for (int a = 1; a < no; a++) {          /* tri par insertion */
		int v = order[a]; int b = a - 1;
		auto key = [&](int i) { return (sceneId < 256 && _lastNext[sceneId] == succ[i]) ? 0u : 1u + cost[i]; };
		while (b >= 0 && key(order[b]) > key(v)) { order[b + 1] = order[b]; b--; }
		order[b + 1] = v;
	}
	uint32 req[256]; int nr = 0; uint32 total = 0;
	const uint32 kBudget = 3u * 1024 * 1024;
	for (int a = 0; a < no; a++) {
		int i = order[a];
		if (total + cost[i] > kBudget && nr > 0) continue;
		for (int k = 0; k < nf[i] && nr < 256; k++) req[nr++] = files[i][k];
		total += cost[i];
	}
	Prefetch::request(req, nr);
#ifndef __DREAMCAST__
	if (BdProf::envFlag("BD_LOADLOG"))
		BdProf::say("PREFETCH scene %u : %d suivantes, %d ressources, %u Ko\n", sceneId, ns, nr, total / 1024);
	while (Prefetch::pump()) {}              /* pas de thread : lecture immédiate */
	static int testJumps = BdProf::envInt("BD_PFTEST", 0);   /* test : on prend vraiment la première sortie, N fois */
	if (testJumps > 0 && ns > 0) {
		testJumps--;
		BdProf::say("TEST saut %u -> %u\n", sceneId, succ[0]);
		_vm->_scene->setSceneId(succ[0]);
		_vm->_scene->loadScene(succ[0], 0x1e);
	}
#endif
}

void SceneHint::dumpAll() {
	BigfileArchive *bf = _vm->getBigfileArchive();
	uint16 n = _vm->_dragonRMS->count();
	for (uint16 s = 2; s < n + 2; s++) {
		uint16 succ[MAX_SUCC];
		int ns = successors(s, succ, MAX_SUCC);
		uint32 files[MAX_FILES];
		int nf = sceneFiles(s, files, MAX_FILES);
		uint32 bytes = 0;
		for (int k = 0; k < nf; k++) bytes += bf->diskSize(files[k]);
		BdProf::say("SCENE %3u (0x%02x) %.4s : %2d fichiers, %5u Ko disque ; suivantes :", s, s, _vm->_dragonRMS->getSceneName(s), nf, bytes / 1024);
		for (int i = 0; i < ns; i++) BdProf::say(" %u", succ[i]);
		BdProf::say("\n");
	}
}

} // End of namespace Dragons
