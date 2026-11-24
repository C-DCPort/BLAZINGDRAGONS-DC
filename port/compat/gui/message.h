#ifndef GUI_MESSAGE_H
#define GUI_MESSAGE_H
#include "common/ustr.h"
namespace GUI {
class MessageDialog {
public:
	MessageDialog(const Common::U32String &, const Common::U32String & = Common::U32String(), const Common::U32String & = Common::U32String()) {}
	int runModal() { return 0; }
};
}
#endif
