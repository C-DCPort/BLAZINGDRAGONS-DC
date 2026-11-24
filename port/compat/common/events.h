/* Événements réduits à ce que le moteur dragons utilise :
   actions de manette (comme sur PS1), souris, molette, quitter. */
#ifndef COMMON_EVENTS_H
#define COMMON_EVENTS_H
#include "common/scummsys.h"
#include "common/rect.h"
namespace Common {
enum EventType {
	EVENT_INVALID = 0, EVENT_QUIT, EVENT_MOUSEMOVE, EVENT_LBUTTONDOWN, EVENT_LBUTTONUP,
	EVENT_RBUTTONDOWN, EVENT_RBUTTONUP, EVENT_WHEELUP, EVENT_WHEELDOWN,
	EVENT_CUSTOM_ENGINE_ACTION_START, EVENT_CUSTOM_ENGINE_ACTION_END
};
typedef uint32 CustomEventType;
struct Event {
	EventType type;
	Point mouse;
	CustomEventType customType;
	Event() : type(EVENT_INVALID), customType(0) {}
};
class EventManager {
public:
	virtual ~EventManager();
	virtual bool pollEvent(Event &event) = 0;
	virtual void pushEvent(const Event &event) = 0;
};
}
#endif
