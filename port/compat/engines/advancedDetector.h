#ifndef ENGINES_ADVANCED_DETECTOR_H
#define ENGINES_ADVANCED_DETECTOR_H
#include "common/language.h"
#include "common/platform.h"
struct ADGameDescription {
	const char *gameId;
	Common::Language language;
	Common::Platform platform;
};
#define AD_GAME_DESCRIPTION_HELPERS(field)
#endif
