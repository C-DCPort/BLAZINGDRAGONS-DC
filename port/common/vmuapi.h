/* Gestion des cartes mémoire (VMU) vue par le moteur. Implémentée par la plate-forme
   (Dreamcast : main_dc.cpp ; simulateur : main_sim.cpp avec de faux VMU). */
#pragma once

namespace Vmu {

enum { kSlots = 3 };                    /* emplacements de sauvegarde par VMU */

void refresh();                         /* relit la liste des VMU (appeler a l'ouverture d'un menu, jamais a chaque image) */
int count();                            /* nombre de VMU présents */
const char *name(int i);                /* « A1 », « B2 »… */
int freeBlocks(int i);                  /* blocs libres, -1 si inconnu */
int savesUsed(int i);                   /* nombre d'emplacements utilisés (0..kSlots) */
bool slotUsed(int i, int slot);         /* slot 0..kSlots-1 */
int active();                           /* VMU qui reçoit les sauvegardes, -1 si aucun */
bool setActive(int i);                  /* déplace aussi le fichier de réglages */
bool copySlot(int from, int to, int slot);
bool eraseSlot(int i, int slot);

}
