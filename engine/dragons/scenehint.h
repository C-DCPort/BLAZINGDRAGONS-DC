/* Port DC : prédit les scènes où le joueur peut aller et demande leur préchargement. */
#ifndef DRAGONS_SCENEHINT_H
#define DRAGONS_SCENEHINT_H

#include "common/scummsys.h"

namespace Dragons {

class DragonsEngine;

class SceneHint {
public:
	explicit SceneHint(DragonsEngine *vm);
	/* À appeler quand une scène est entièrement chargée. */
	void onSceneLoaded(uint16 sceneId);
	void dumpAll();                    /* simulateur : graphe des scènes et volumes (BD_SCENEDUMP) */

private:
	enum { MAX_SUCC = 24, MAX_FILES = 48 };
	DragonsEngine *_vm;
	uint16 _prev;
	uint16 _lastNext[256];             /* dernière scène visitée après chacune (appris en jouant) */

	bool validScene(uint32 sceneId) const;
	int successors(uint16 sceneId, uint16 *out, int cap);
	int sceneFiles(uint16 sceneId, uint32 *ids, int cap);
};

} // End of namespace Dragons

#endif
