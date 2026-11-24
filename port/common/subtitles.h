/* Sous-titres des videos : fichier <video>.sub (UTF-8), une ligne par sous-titre :
       debut fin texte        ex :  00:03.500 00:06.000 Bonjour|petit dragon
   Temps en mm:ss.mmm (ou ss.mmm) depuis le debut de la video ; '|' coupe la ligne ; les lignes commençant par # sont ignorees. */
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Subs {

struct Cue { uint32_t startMs, endMs; char text[160]; };

class Track {
public:
	Track() : _cues(nullptr), _n(0), _last(0) {}
	~Track();
	bool parse(const char *data, size_t size);          /* renvoie false si aucun sous-titre */
	const char *textAt(uint32_t ms);                    /* nullptr si aucun sous-titre a cet instant */
	int count() const { return _n; }
private:
	Cue *_cues; int _n; int _last;
};

/* Dessine le texte (centre, bas du cadre, contour noir) dans un tampon RGB 16 bits w x h. Renvoie le nombre de lignes. */
int drawText(uint16_t *px, int pitchPixels, int w, int h, const char *utf8, uint16_t fg, uint16_t outline);
int textHeight(const char *utf8);                       /* hauteur en pixels du texte (lignes * hauteur de ligne) */

}
