// Lecture disque par gros blocs alignés sur les secteurs.
//
// Sur Dreamcast, fs_iso9660 ne lance une lecture multi-secteurs DMA que si l'on tombe sur une limite
// de secteur ET que le tampon est aligné sur 32 octets ; sinon chaque secteur de 2 Ko passe par une
// commande GD-ROM séparée (très lent, aussi bien sur console que dans Flycast). Ce module lit donc
// toujours des secteurs entiers dans un tampon aligné, puis recopie la partie demandée.
// Sur PC (simulateur) c'est un simple fseek/fread.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Disc {
struct File;

void setDataFallback(const char *dir);              /* dossier de repli (fichiers absents du premier) */
void setDataDir(const char *dir);                  /* dossier des données (appelé par Common::setGameDataPath) */
File *open(const char *name, bool background = false);   /* nom cherché dans le dossier, casse ignorée ; nullptr si absent */
void close(File *f);
long size(File *f);
/* Lit n octets à partir de off (n'importe quel alignement). Renvoie le nombre d'octets lus. */
long readAt(File *f, uint32_t off, void *dst, uint32_t n);

/* Un fichier « background » (préchargement) cède la place : il attend entre deux blocs tant qu'une lecture
   de premier plan est en cours. */
void setSleepHook(void (*sleepMs)(int));
extern unsigned long long statCommands;            /* nombre de lectures multi-secteurs lancées */
extern unsigned long long statBytes;               /* octets réellement transférés (secteurs entiers) */
}
