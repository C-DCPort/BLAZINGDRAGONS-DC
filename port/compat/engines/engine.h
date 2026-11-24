/* Version réduite de la classe Engine de ScummVM pour le portage Dreamcast. */
#ifndef ENGINES_ENGINE_H
#define ENGINES_ENGINE_H
#include "common/scummsys.h"
#include "common/error.h"
#include "common/str.h"
#include "common/ustr.h"
#include "common/language.h"
#include "common/platform.h"
#include "common/stream.h"
#include "common/system.h"

namespace Graphics { struct Surface; struct PixelFormat; }
void GUIErrorMessage(const Common::U32String &msg, const char *url = nullptr);
void GUIErrorMessageWithURL(const Common::U32String &msg, const char *url);

class OSystem;
namespace Audio { class Mixer; }
namespace Common { class EventManager; class SaveFileManager; }

class Engine {
public:
	enum EngineFeature {
		kSupportsReturnToLauncher, kSupportsLoadingDuringRuntime, kSupportsSavingDuringRuntime,
		kSupportsChangingOptionsDuringRuntime, kSupportsSubtitleOptions
	};
	explicit Engine(OSystem *syst);
	virtual ~Engine() {}
	virtual Common::Error run() = 0;
	virtual bool hasFeature(EngineFeature f) const { return false; }
	virtual Common::Error loadGameState(int slot) { return Common::kNoError; }
	virtual bool canLoadGameStateCurrently(Common::U32String *msg = nullptr) { return false; }
	virtual Common::Error saveGameState(int slot, const Common::String &desc, bool isAutosave = false) { return Common::kNoError; }
	virtual bool canSaveGameStateCurrently(Common::U32String *msg = nullptr) { return false; }
	virtual void syncSoundSettings() {}
	virtual Common::String getSaveStateName(int slot) const;

	static bool shouldQuit();
	static void quitGame();
	static void clearQuit();   /* port DC : fin du jeu -> retour au menu titre sans relancer le programme */
	void pauseEngine(bool) {}
	int openMainMenuDialog() { return 0; }
	uint32 getTotalPlayTime() const;
	void setTotalPlayTime(uint32 time = 0);
	OSystem *getSystem() { return _system; }
	Common::SaveFileManager *getSaveFileManager() { return _saveFileMan; }
	const Common::String &getTargetName() const { return _targetName; }
	void setDebugger(void *) {}

protected:
	OSystem *_system;
	Audio::Mixer *_mixer;
	Common::EventManager *_eventMan;
	Common::SaveFileManager *_saveFileMan;
	Common::String _targetName;
	uint32 _engineStartTime = 0;
};

extern Engine *g_engine;
#endif
