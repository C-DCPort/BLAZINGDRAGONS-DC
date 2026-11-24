/* Dessin de la manette Dreamcast pour l'écran « Contrôles » (RGB555, bit 15 = opaque). */
#pragma once
#include <stdint.h>

namespace PadArt {

enum { W = 320, H = 200, kLegendY = 128 };   /* kLegendY : ordonnée de la 1re ligne de légende (ligne de texte 16) */

/* assign[f] : bouton (InputMap::BTN_*) de chaque fonction ; sel : fonction choisie (-1 = aucune) ;
   blink : 0/1 pour faire clignoter l'anneau de la fonction choisie. */
void draw(uint16_t *px, int pitchPx, const uint32_t *assign, int nFn, int sel, int blink);

/* Petite manette sur fond transparent (concours de danse) : litBit = bouton à presser (InputMap::BTN_*, 0 = aucun),
   entouré d'un anneau blanc. Dessin réduit dans dst (dstW × dstH, RGB555 avec bit 15 = opaque, 0 = transparent). */
void drawSmall(uint16_t *dst, int dstPitchPx, int dstW, int dstH, uint32_t litBit);

/* Même chose dans le style de la manette PlayStation d'origine (contour seul, penchée, boule bleue sur le bouton). */
void drawDance(uint16_t *dst, int dstPitchPx, int dstW, int dstH, uint32_t litBit);

/* Couleur (RGB555 avec bit 15) d'une fonction, pour la légende. */
uint16_t fnColour(int f);

}
