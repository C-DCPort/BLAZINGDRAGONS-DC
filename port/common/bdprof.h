// Chronomètres légers pour le journal (désactivés si aucune horloge n'est fournie).
#pragma once
namespace BdProf {
enum { DISC, TEX, PRESENT, WAIT, PREP, COUNT };
extern unsigned long long (*nowFn)();
extern unsigned long long ms[COUNT];
extern unsigned n[COUNT];
extern unsigned long long discBytes;
/* Journal des chargements depuis le BIGFILE (nom, taille, durée) */
struct LoadRec { char name[16]; unsigned size; unsigned ms; };
extern LoadRec loads[48];
extern unsigned nLoads;                 /* nombre depuis la dernière remise à zéro */
extern bool echoLoads;                  /* affiche chaque chargement sur stderr (simulateur) */
int envInt(const char *name, int def);     /* variable d'environnement entière (simulateur) */
bool envFlag(const char *name);          /* variable d'environnement définie (simulateur) */
void say(const char *fmt, ...);           /* message de diagnostic (stderr) */
inline void copy(void *d, const void *s, unsigned n) { __builtin_memcpy(d, s, n); }
void logLoad(const char *name, unsigned size, unsigned ms, bool cached = false);
/* détail du temps de préparation GPU par type d'appel (journal) */
enum { S_SPRITE, S_BMP16, S_VIDEO, S_FILL, S_LAYER, S_HDLAYER, S_HDSPRITE, S_WRAPY, S_COUNT };
extern unsigned long long subMs[S_COUNT];
extern unsigned subN[S_COUNT];
struct Sub {
	int id; unsigned long long t;
	explicit Sub(int i) : id(i), t(nowFn ? nowFn() : 0) {}
	~Sub() { if (nowFn) { subMs[id] += nowFn() - t; subN[id]++; } }
};
struct Scope {
	int id; unsigned long long t;
	explicit Scope(int i) : id(i), t(nowFn ? nowFn() : 0) {}
	~Scope() { if (nowFn) { ms[id] += nowFn() - t; n[id]++; } }
};
}
