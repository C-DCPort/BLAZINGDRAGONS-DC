#ifndef COMMON_CONFIG_MANAGER_H
#define COMMON_CONFIG_MANAGER_H
#include "common/str.h"
namespace Common {
class ConfigManager {
public:
	bool hasKey(const String &key) const;
	int getInt(const String &key) const;
	bool getBool(const String &key) const;
	void set(const String &key, const String &value);
	void setInt(const String &key, int value);
	void setBool(const String &key, bool value);
	String get(const String &key) const;
	void registerDefault(const String &, bool) {}
	void registerDefault(const String &, int) {}
	void registerDefault(const String &, const char *) {}
	void flushToDisk() {}
};
}
extern Common::ConfigManager g_confMan;
#define ConfMan g_confMan
#endif
