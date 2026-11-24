#!/usr/bin/env python3
"""Corrige des fautes d'orthographe des dialogues français dans BIGFILE.DAT (fichiers drag*.txt).

usage : text_fix.py BIGFILE.DAT [--list]   (modifie le fichier sur place ; --list : affiche sans rien écrire)

Les répliques sont repérées par leur position dans le fichier : chaque réplique garde exactement sa longueur.
Une correction plus longue prend la place d'un double espace de la même réplique (les phrases sont séparées par
deux espaces) ou, à défaut, de l'espace de tête ; une plus courte en ajoute un après une ponctuation (ou un espace final). Si c'est impossible, la
correction est sautée et signalée.
Les fautes volontaires (accent italien de la gondolière : « Céla mé rapelle », « esploser »… ; latin de cuisine de
Mervin : « arrêtus maintenanum » ; noms comme « Lanç-mon-salaud ») ne sont pas touchées.
"""
import csv, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
# codes de la police (proches de la page de code 437)
ENC = {'Ç': 128, 'é': 130, 'â': 131, 'ä': 132, 'à': 133, 'ç': 135, 'ê': 136, 'ë': 137, 'è': 138, 'ï': 139, 'î': 140,
       'É': 144, 'ô': 147, 'û': 150, 'ù': 151}
DEC = {v: k for k, v in ENC.items()}

FIXES = [   # (ancien, nouveau) : mots entiers
    ("aggressif", "agressif"), ("attrapper", "attraper"), ("Aujourdh'ui", "Aujourd'hui"), ("crapeaux", "crapauds"),
    ("dinstinguer", "distinguer"), ("disgestion", "digestion"), ("dégôutez", "dégoûtez"), ("intéresée", "intéressée"),
    ("mirroir", "miroir"), ("monstuosité", "monstruosité"), ("orginale", "originale"), ("paysants", "paysans"),
    ("probabe", "probable"), ("sensasionnelles", "sensationnelles"), ("suprise", "surprise"), ("sureté", "sûreté"),
    ("tourisitique", "touristique"), ("éssai", "essai"), ("hâchée", "hachée"), ("gène", "gêne"),
    ("retraire", "retraite"), ("Echanteur", "Enchanteur"), ("gemtil", "gentil"), ("chèrie", "chérie"),
    ("crome", "chrome"), ("Flamèche", "Flammèche"), ("Choisit un chapeau", "Choisis un chapeau"),
    ("Fait face à", "Fais face à"), ("chateau", "château"),
    ("!.Unpéage  construit", "! Un péage construit"),
]

def decode(d, i):
    out = []
    while i + 1 < len(d):
        c = struct.unpack_from('<H', d, i)[0]
        if c == 0: break
        out.append(c); i += 2
    return out

def to_str(codes):
    return ''.join(DEC.get(c, chr(c)) for c in codes)

def to_codes(s):
    return [ENC[ch] if ch in ENC else ord(ch) for ch in s]

def fit(s, n):
    """Ramène s à n caractères en jouant sur les espaces."""
    while len(s) > n:
        k = s.find('  ')
        if k < 0:
            if s.startswith(' '): s = s[1:]; continue   # en dernier recours : l'espace de tête des répliques
            return None
        s = s[:k] + s[k + 1:]
    while len(s) < n:
        m = re.search(r'([.!?]) (?! )', s)
        if m: s = s[:m.end()] + ' ' + s[m.end():]
        else: s = s + ' '
    return s

def main():
    path = sys.argv[1]; dry = '--list' in sys.argv
    rows = list(csv.reader(open(os.path.join(HERE, '../docs/bigfile_fr.csv')), delimiter=';'))[1:]
    ent = {r[1].lower(): (int(r[2]), int(r[3])) for r in rows}
    big = bytearray(open(path, 'rb').read())
    n_fix = n_skip = 0
    for name, (o, size) in sorted(ent.items()):
        if not (name.startswith('drag') and name.endswith('.txt')): continue
        i = o + 10; end = o + size
        while i + 1 < end:
            codes = decode(big[:end], i)
            if codes and all(c >= 0x20 for c in codes):   # répliques sans codes de contrôle seulement
                s = to_str(codes)
                t = s
                for a, b in FIXES:
                    t = re.sub(r'(?<![\wÀ-ÿ])' + re.escape(a) + r'(?![\wÀ-ÿ])', b, t)
                if t != s:
                    t2 = fit(t, len(s))
                    if t2 is None or any(ch not in ENC and ord(ch) > 127 for ch in t2):
                        print('SAUTE  %s +%x : %s' % (name, i - o - 10, t)); n_skip += 1
                    else:
                        print('CORRIGE %s +%x : %s' % (name, i - o - 10, t2.strip())); n_fix += 1
                        if not dry:
                            for k, c in enumerate(to_codes(t2)):
                                struct.pack_into('<H', big, i + 2 * k, c)
            i += 2 * (len(codes) + 1)
    if not dry and n_fix: open(path, 'wb').write(big)
    print('textes : %d répliques corrigées, %d sautées' % (n_fix, n_skip))

if __name__ == '__main__':
    main()
