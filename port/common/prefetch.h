// Préchargement en arrière-plan des ressources de BIGPACK.DAT.
//
// Pendant que le joueur est dans une scène, un thread lit (compressées) les ressources des scènes où il
// peut aller ensuite et les garde en mémoire. BigfileArchive::load les prend en priorité, ce qui évite
// l'attente disque au changement de scène. Les données restent compressées (3 fois moins de mémoire).
// Sur PC (simulateur) il n'y a pas de thread : le code appelle pump() lui-même.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Prefetch {
struct Env {
	unsigned (*freeKB)();        /* mémoire encore disponible (Ko) ; nullptr = illimitée */
	void (*lock)();              /* exclusion mutuelle entre le thread de lecture et le jeu */
	void (*unlock)();
};
void setEnv(const Env &e);
/* table : par ressource {offset, taille compressée, taille d'origine, méthode} (propriété de l'appelant) */
void configure(const char *packName, const uint32_t *table, uint32_t count);
void shutdown();
/* Remplace la liste à précharger (ordre = priorité). Ce qui n'est plus demandé est libéré. */
void request(const uint32_t *ids, uint32_t n);
/* Renvoie le bloc compressé (à libérer avec free) ou nullptr s'il n'est pas prêt. */
uint8_t *take(uint32_t id, uint32_t *csize);
bool has(uint32_t id);
/* Lit un morceau du travail en cours. Renvoie false s'il n'y a rien à faire (ou plus de place). */
bool pump();
void flush();                    /* libère tout (manque de mémoire) */
size_t storedBytes();
extern unsigned statHit, statMiss, statLoaded;     /* utilisations, demandes non prêtes, ressources lues en avance */
extern unsigned long long statBytesLoaded;
}
