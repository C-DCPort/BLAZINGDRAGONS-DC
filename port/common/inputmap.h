/* Correspondance boutons de la manette -> fonctions du jeu (écran « Contrôles »).
   Les bits sont indépendants de la plate-forme ; la Dreamcast les traduit depuis KOS. */
#pragma once
#include <stdint.h>

namespace InputMap {

enum { BTN_A = 1, BTN_B = 2, BTN_X = 4, BTN_Y = 8, BTN_L = 16, BTN_R = 32 };
enum { F_ACTION = 0, F_INVENTORY = 1, F_SCROLL_UP = 2, F_SCROLL_DOWN = 3, F_COUNT = 4 };

extern uint32_t assign[F_COUNT];        /* bouton (un bit) de chaque fonction */
extern uint32_t pressedNow;             /* boutons pressés depuis le dernier takePressed() (renseigné par la plate-forme) */

void resetDefaults();
uint32_t takePressed();
const char *buttonName(uint32_t bit);
/* Donne le bouton b à la fonction f ; si une autre fonction l'avait, elle prend l'ancien bouton de f (échange). */
void set(int f, uint32_t bit);

}
