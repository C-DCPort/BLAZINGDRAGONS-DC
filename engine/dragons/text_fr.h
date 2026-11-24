/* Port Dreamcast : répliques restées en anglais sur le disque français (fin du jeu : le roi et Mervin pendus à l'arbre,
   le retour au château, le coup de téléphone de Trépied ; quelques messages génériques). Traduction ajoutée, affichée à
   la place du texte anglais quand le jeu est en français. Les voix restent celles du disque.
   Texte en UTF-8, converti à l'affichage vers le jeu de caractères de la police (accents en codes 0x80+, comme le disque). */
#ifndef DRAGONS_TEXT_FR_H
#define DRAGONS_TEXT_FR_H

namespace Dragons {

struct TextFr { unsigned int index; const char *text; };

static const TextFr kTextFr[] = {
	{ 0x0214a, " Je ne peux pas regarder \xc3\xa7" "a!" },
	{ 0x02182, " Je ne peux pas parler \xc3\xa0 \xc3\xa7" "a!" },
	{ 0x021ba, " Je ne peux pas utiliser \xc3\xa7" "a!" },
	{ 0x021ea, " Quelque chose me bloque le passage." },
	{ 0x0223e, " Je ne peux pas utiliser ces objets ensemble." },
	{ 0x02290, " Je ne peux pas prendre \xc3\xa7" "a." },
	{ 0x1bbd0, " Bonjour.  C'est encore moi." },
	{ 0x1bc08, " Viandes et gibier?  Essayez le rayon 6." },
	{ 0x2425e, " Pr\xc3\xaat et dispos\xc3\xa9, Roi Artifur." },
	{ 0x57f1c, " Mervin, esp\xc3\xa8" "ce de parfait imb\xc3\xa9" "cile!" },
	{ 0x58000, " Imb\xc3\xa9" "cile?  Esp\xc3\xa8" "ce de cr\xc3\xa9tin ingrat et bouffi!  C'est moi qui t'ai fait!  Tu es l\xc3\xa0 o\xc3\xb9 tu es aujourd'hui gr\xc3\xa2" "ce \xc3\xa0 moi!" },
	{ 0x580c8, " Alors merci de m'avoir perch\xc3\xa9 dans un arbre, esp\xc3\xa8" "ce d'andouille travestie!" },
	{ 0x58158, " Ouah..." },
	{ 0x58174, " Quelle chance, Sire -- atterrir comme \xc3\xa7" "a pile sur la manufacture royale de matelas." },
	{ 0x58232, " Oh, je ne me suis pas senti aussi jeune depuis des ann\xc3\xa9" "es.  Je crois que j'en ai encore pour cinq cents ans de r\xc3\xa8gne.  Dis-moi, Flamm\xc3\xa8" "che, si tu ne voulais pas vraiment diriger le royaume, pourquoi t'es-tu inscrit au tournoi?" },
	{ 0x583da, " Eh bien, Sire, \xc3\xa0 part les raisons habituelles de bravade machiste et de b\xc3\xaatise g\xc3\xa9n\xc3\xa9rale, c'\xc3\xa9tait pour \xc3\xa9viter \xc3\xa0 la princesse Flamme d'\xc3\xa9pouser quelqu'un contre son gr\xc3\xa9." },
	{ 0x58512, " Je vois.  Flamm\xc3\xa8" "che, ta conduite ces derniers jours a montr\xc3\xa9 que tu es un gar\xc3\xa7on au noble caract\xc3\xa8re, d'un courage qui frise le suicidaire.  Je crois que tu ferais le pr\xc3\xa9tendant id\xc3\xa9" "al pour ma Flamme." },
	{ 0x586b4, " C'est tout ce que j'avais \xc3\xa0 faire pour vous impressionner?  J'aurais fait \xc3\xa7" "a il y a des ann\xc3\xa9" "es." },
	{ 0x58756, " \xc3\x87" "a a l'air dr\xc3\xb4lement long pour retourner au ch\xc3\xa2teau..." },
	{ 0x587ca, " Oui, je ne me souvenais pas que c'\xc3\xa9tait si loin..." },
	{ 0x5882c, " On dirait qu'on marche sur un tapis roulant." },
	{ 0x5888c, " Je crois que j'ai compris, Flamm\xc3\xa8" "che.  On a pris un raccourci." },
	{ 0x58906, " Un raccourci?" },
	{ 0x5892a, " Oui, je parie que les graphistes ont pris un raccourci en faisant tourner cette route en boucle sur 20 images." },
	{ 0x589ce, " \xc3\x87" "a explique tout.  Vous vous rendez compte du culot de ces types?" },
	{ 0x58a58, " Ouais, Murray?  C'est moi.  Colle bien le t\xc3\xa9l\xc3\xa9phone \xc3\xa0 ton oreille, mon pote.  J'ai un truc tr\xc3\xa8s personnel \xc3\xa0 te dire..." },
	{ 0x58b38, " T'ES VIRE!  Tu pues!  T'es pas un agent, t'es une mal\xc3\xa9" "diction!" },
	{ 0x58bc4, " Je vais te dire ce qui ne va pas.  C'est ce boulot que tu m'as trouv\xc3\xa9.  Celui avec les dragons, les chevaliers et toutes ces \xc3\xa2neries.  C'est un navet, Murray." },
	{ 0x58cd0, " Et cette histoire d'ordinateurs et de jeux interactifs, ce n'est pas la vague de l'avenir, Buck Rogers.  C'est la vague qui rejette les poissons morts sur la plage.  \xc3\x87" "a pue encore plus que toi et \xc3\xa7" "a ne prendra jamais." },
	{ 0x59000, " Tout ce qu'on fait, c'est se balader en ramassant de quoi remplir un caddie.  Oh, et \xc3\xa9" "coute \xc3\xa7" "a -- le sac qu'on te donne pour tout ranger est MINUSCULE.  Te voil\xc3\xa0 en train de trimballer une demi-tonne de bric-\xc3\xa0-brac inutile que tu ne pourrais m\xc3\xaame pas soulever avec une fichue grue." },
	{ 0x59216, " Suspendez votre incr\xc3\xa9" "dulit\xc3\xa9, si vous le voulez bien, les amis.  Vous vous cassez le dos \xc3\xa0 chercher des rognures d'ongles de pied qui vous serviront plus tard, avec des peluches de nombril, \xc3\xa0 fabriquer une bombe atomique." },
	{ 0x59394, " Ouais!  Oh, et \xc3\xa9" "coute \xc3\xa7" "a!  S'il y a un probl\xc3\xa8me que les programmeurs n'ont pas envie de r\xc3\xa9gler, ils disent juste que \xc3\xa7" "a a des \"propri\xc3\xa9t\xc3\xa9s magiques\" et passent \xc3\xa0 la sc\xc3\xa8ne suivante." },
	{ 0x5950a, " Qui \xc3\xa9" "crit ces trucs, nom d'une pipe?  Oh, et en plus ils coupent la moiti\xc3\xa9 de mes r\xc3\xa9pliques pour donner plus de temps \xc3\xa0 l'\xc3\xa9" "cran \xc3\xa0 ce gamin de Flamm\xc3\xa8" "che, qui, entre nous, est nul." },
	{ 0x59668, " Je m'en fiche, Murray!  Et si tu commen\xc3\xa7" "ais \xc3\xa0 gagner les 20% que tu me voles en me trouvant du boulot \xc3\xa0 la t\xc3\xa9l\xc3\xa9?  Tiens, une id\xc3\xa9" "e -- mets-moi avec de jeunes beaut\xc3\xa9s et on r\xc3\xa9soudra des crimes.  Les Anges de Tr\xc3\xa9pied, un truc comme \xc3\xa7" "a." },
	{ 0x59838, " Bon, faut que j'y aille avant que ces patates se mettent \xc3\xa0 travailler sur une suite." },
	{ 0x598c2, " Hein?  Je me fiche que mon contrat dise que je dois le faire.  Je ne viendrai pas, c'est tout.  Qu'est-ce qu'ils vont faire, me faire un proc\xc3\xa8s?" },
	{ 0x599a2, " Oh.  Ouais, bien vu.  Bon, je serai dans le prochain, mais seulement s'ils prennent quelqu'un qui a au moins son bac pour faire les graphismes.  Occupe-t'en." },
	{ 0x59ae8, " OK, ch\xc3\xa9ri, je me tire.  Si tu me cherches, je serai \xc3\xa0 Two Bunch pour un bain et un enveloppement de boue.  D'accord.  Moi aussi, je t'aime." },
};

/* UTF-8 -> caractères de la police du jeu (codes du disque français : é 0x82, è 0x8a, ê 0x88, à 0x85, ç 0x87, â 0x83,
   ô 0x93, ù 0x97, î 0x8c, ë 0x89, ï 0x8b, Ç 0x80). Renvoie la longueur écrite (sans le 0 final). */
static inline unsigned int textFrEncode(const char *s, unsigned short *dst, unsigned int dstLen) {
	unsigned int n = 0;
	const unsigned char *p = (const unsigned char *)s;
	while (*p && n + 1 < dstLen) {
		unsigned short c = *p++;
		if (c == 0xc3 && *p) {
			const unsigned char b = *p++;
			switch (b) {
			case 0xa9: c = 0x82; break; case 0xa8: c = 0x8a; break; case 0xaa: c = 0x88; break; case 0xa0: c = 0x85; break;
			case 0xa7: c = 0x87; break; case 0xa2: c = 0x83; break; case 0xb4: c = 0x93; break; case 0xb9: c = 0x97; break;
			case 0xae: c = 0x8c; break; case 0xab: c = 0x89; break; case 0xaf: c = 0x8b; break; case 0x87: c = 0x80; break;
			default: c = '?'; break;
			}
		}
		dst[n++] = c;
	}
	dst[n] = 0;
	return n;
}

static inline const char *textFrFind(unsigned int index) {
	for (unsigned int i = 0; i < sizeof(kTextFr) / sizeof(kTextFr[0]); i++) if (kTextFr[i].index == index) return kTextFr[i].text;
	return nullptr;
}

} // namespace Dragons

#endif
