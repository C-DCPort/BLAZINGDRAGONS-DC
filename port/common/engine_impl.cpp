#define FORBIDDEN_SYMBOL_ALLOW_ALL
/* Mise en œuvre de la couche compat commune au PC et à la Dreamcast :
   Engine, ConfMan, fichiers, messages d'erreur. */
#include "bdprof.h"
#include "disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <map>
#include <string>
#include "engines/engine.h"
#include "engines/util.h"
#include "common/config-manager.h"
#include "common/file.h"
#include "common/events.h"
#include "common/textconsole.h"
#include "common/debug.h"

Engine *g_engine = nullptr;
OSystem *g_system = nullptr;
Common::ConfigManager g_confMan;
static bool s_quit = false;

Engine::Engine(OSystem *syst) : _system(syst), _mixer(nullptr), _eventMan(nullptr), _saveFileMan(nullptr) {
	g_engine = this;
	_eventMan = syst->getEventManager();
	_saveFileMan = syst->getSavefileManager();
	_engineStartTime = syst->getMillis();
}
bool Engine::shouldQuit() { return s_quit; }
void Engine::quitGame() { s_quit = true; }
void Engine::clearQuit() { s_quit = false; }
Common::String Engine::getSaveStateName(int slot) const { return Common::String::format("bd.%03d", slot); }
uint32 Engine::getTotalPlayTime() const { return g_system->getMillis() - _engineStartTime; }
void Engine::setTotalPlayTime(uint32 t) { _engineStartTime = g_system->getMillis() - t; }

void GUIErrorMessage(const Common::U32String &msg, const char *) { fprintf(stderr, "GUI: %s\n", msg.encode().c_str()); }
void GUIErrorMessageWithURL(const Common::U32String &msg, const char *url) { GUIErrorMessage(msg, url); }
void initGraphics(int, int, const Graphics::PixelFormat *) {}

void OSystem::getTimeAndDate(TimeDate &t, bool) const {
	time_t now = time(nullptr);
	struct tm *tm = localtime(&now);
	t.tm_sec = tm->tm_sec; t.tm_min = tm->tm_min; t.tm_hour = tm->tm_hour;
	t.tm_mday = tm->tm_mday; t.tm_mon = tm->tm_mon; t.tm_year = tm->tm_year; t.tm_wday = tm->tm_wday;
}

/* --- ConfMan : simple table clé → valeur --- */
static std::map<std::string, std::string> s_conf;
namespace Common {
bool ConfigManager::hasKey(const String &k) const { return s_conf.count(k.c_str()) != 0; }
String ConfigManager::get(const String &k) const { auto it = s_conf.find(k.c_str()); return it == s_conf.end() ? String() : String(it->second.c_str()); }
int ConfigManager::getInt(const String &k) const { return atoi(get(k).c_str()); }
bool ConfigManager::getBool(const String &k) const { String v = get(k); return v == "true" || v == "1"; }
void ConfigManager::set(const String &k, const String &v) { s_conf[k.c_str()] = v.c_str(); }
void ConfigManager::setInt(const String &k, int v) { s_conf[k.c_str()] = std::to_string(v); }
void ConfigManager::setBool(const String &k, bool v) { s_conf[k.c_str()] = v ? "true" : "false"; }

/* --- Fichiers : noms insensibles à la casse, cherchés dans le dossier des données --- */
static std::string s_dataPath = ".";
void setGameDataPath(const char *p) { s_dataPath = p; Disc::setDataDir(p); }

/* dossier de repli : version anglaise dans /disc/US, les fichiers communs (vidéos…) restent dans /disc */
static std::string s_dataFallback;
void setGameDataFallback(const char *p) { s_dataFallback = p ? p : ""; Disc::setDataFallback(p); }
static FILE *openCI1(const std::string &dir, const char *name);
static FILE *openCI(const char *name) {
	FILE *f = openCI1(s_dataPath, name);
	if (!f && !s_dataFallback.empty()) f = openCI1(s_dataFallback, name);
	return f;
}
static FILE *openCI1(const std::string &dir, const char *name) {
	std::string base = dir + "/";
	std::string n = name;
	FILE *f = fopen((base + n).c_str(), "rb");
	if (f) return f;
	for (auto &c : n) c = toupper((unsigned char)c);
	f = fopen((base + n).c_str(), "rb");
	if (f) return f;
	for (auto &c : n) c = tolower((unsigned char)c);
	return fopen((base + n).c_str(), "rb");
}

bool File::open(const char *filename) {
	BdProf::Scope prof(BdProf::DISC);
	close();
	FILE *f = openCI(filename);
	if (!f) return false;
	fseek(f, 0, SEEK_END); _size = ftell(f); fseek(f, 0, SEEK_SET);
	_f = f; _eos = false;
	return true;
}
bool File::open(const Path &p) { return open(p.toString().c_str()); }
void File::close() { if (_f) fclose((FILE *)_f); _f = nullptr; _size = 0; }
bool File::exists(const Path &p) { FILE *f = openCI(p.toString().c_str()); if (f) fclose(f); return f != nullptr; }
uint32 File::read(void *buf, uint32 n) {
	BdProf::Scope prof(BdProf::DISC);
	uint32 r = fread(buf, 1, n, (FILE *)_f);
	BdProf::discBytes += r;
	if (r < n) _eos = true;
	return r;
}
int64 File::pos() const { return ftell((FILE *)_f); }
bool File::seek(int64 off, int whence) { BdProf::Scope prof(BdProf::DISC); _eos = false; return fseek((FILE *)_f, off, whence) == 0; }

EventManager::~EventManager() {}
}

/* --- Messages --- */
static void vout(const char *tag, const char *fmt, va_list ap) {
	fprintf(stderr, "%s", tag); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n");
}
void NORETURN_PRE error(const char *fmt, ...) {
	va_list ap; va_start(ap, fmt); vout("ERREUR: ", fmt, ap); va_end(ap);
	abort();
}
void warning(const char *fmt, ...) {
	if (!getenv("BD_VERBOSE")) return;
	va_list ap; va_start(ap, fmt); vout("attention: ", fmt, ap); va_end(ap);
}
void debug(const char *fmt, ...) {
	if (!getenv("BD_DEBUG")) return;
	va_list ap; va_start(ap, fmt); vout("", fmt, ap); va_end(ap);
}
void debug(int level, const char *fmt, ...) {
	if (!getenv("BD_DEBUG") || level > atoi(getenv("BD_DEBUG"))) return;
	va_list ap; va_start(ap, fmt); vout("", fmt, ap); va_end(ap);
}
void debugN(int, const char *, ...) {}
void debugN(const char *, ...) {}
void debugC(int, uint32, const char *, ...) {}
void debugC(uint32, const char *, ...) {}
void debugCN(int, uint32, const char *, ...) {}
void debugCN(uint32, const char *, ...) {}

/* Le moteur tourne dans un seul fil : verrous vides. */
#include "common/mutex.h"
namespace Common {
Mutex::Mutex() : _mutex(nullptr) {}
Mutex::~Mutex() {}
bool Mutex::lock() { return true; }
bool Mutex::unlock() { return true; }
StackLock::StackLock(MutexInternal *m, const char *n) : _mutex(m), _mutexName(n) {}
StackLock::StackLock(const Mutex &, const char *n) : _mutex(nullptr), _mutexName(n) {}
StackLock::~StackLock() {}
}

namespace BdProf {
LoadRec loads[48]; unsigned nLoads = 0; bool echoLoads = false;
void logLoad(const char *name, unsigned size, unsigned ms, bool cached) {
	if (echoLoads) fprintf(stderr, "%s %-16s %8u\n", cached ? "CACHE " : "CHARGE", name, size);
	LoadRec &r = loads[nLoads % 48];
	strncpy(r.name, name, 15); r.name[15] = 0; r.size = size; r.ms = ms; nLoads++;
}
int envInt(const char *name, int def) { const char *v = getenv(name); return v ? atoi(v) : def; }
bool envFlag(const char *name) { return getenv(name) != nullptr; }
void say(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); }
unsigned long long (*nowFn)() = nullptr;
unsigned long long subMs[S_COUNT];
unsigned subN[S_COUNT];
unsigned long long ms[COUNT];
unsigned n[COUNT];
unsigned long long discBytes;
}
