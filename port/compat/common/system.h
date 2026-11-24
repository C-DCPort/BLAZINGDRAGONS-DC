/* OSystem réduit : écran 16 bits, temps, événements, sauvegardes.
   Mis en œuvre par port/sim (PC) ou port/dc (Dreamcast). */
#ifndef COMMON_SYSTEM_H
#define COMMON_SYSTEM_H
#include "common/scummsys.h"
#include "common/str.h"

namespace Common { class EventManager; class SaveFileManager; }
namespace Graphics { struct Surface; }

struct TimeDate { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday; };

class OSystem {
public:
	virtual ~OSystem() {}
	bool backendInitialized() const { return true; }
	virtual uint32 getMillis(bool skipRecord = false) = 0;
	virtual void delayMillis(uint msecs) = 0;
	virtual void copyRectToScreen(const void *buf, int pitch, int x, int y, int w, int h) = 0;
	virtual void fillScreen(uint32 col) = 0;
	virtual void updateScreen() = 0;
	virtual void setPalette(const byte *, uint, uint) {}
	virtual void getTimeAndDate(TimeDate &t, bool skipRecord = false) const;
	virtual Common::EventManager *getEventManager() = 0;
	virtual Common::SaveFileManager *getSavefileManager() = 0;
};

extern OSystem *g_system;
#endif
