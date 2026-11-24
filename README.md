# Blazing Dragons — portage Dreamcast (HD)

Portage natif Dreamcast (KallistiOS, rendu PVR direct) de *Blazing Dragons* (PlayStation, 1996, Crystal Dynamics).
La logique du jeu est reprise du moteur `dragons` de ScummVM (GPL).

Projet de fans, gratuit et non officiel, sans lien avec Crystal Dynamics ni avec les ayants droit du jeu.
**Aucune donnée du jeu n'est fournie** : il faut posséder sa propre copie PlayStation.

- En français, en anglais, ou les deux sur le même disque (choix dans Options > Langue)
- Graphismes HD en option : décors redessinés et animations des personnages
- Sauvegardes sur VMU
- Testé sur Flycast et sur console (DreamShell)

## Ce qu'il faut

1. **Le jeu PlayStation**, en image disque `.bin` / `.cue`, au moins une des deux versions :
   - la version française (SLES_003.05) ;
   - la version américaine.

   Avec les deux, le disque est bilingue. Avec une seule, il est dans la langue de cette version.
2. **Les graphismes HD**, facultatifs : deux fichiers `HD.DAT` (décors) et `HDSPR.DAT` (sprites), à télécharger dans release. Sans eux, le jeu fonctionne avec les graphismes d'origine.
3. **Un environnement Linux**, ou WSL sous Windows, avec :
   - la chaîne de compilation Dreamcast [KallistiOS](https://github.com/KallistiOS/KallistiOS), dans `/opt/toolchains/dc` ;
   - `python3` ;
   - `git` ;
   - [`mkdcdisc`](https://github.com/KallistiOS/mkdcdisc), pour l'image CDI (Flycast, GDEMU) ;
   - `genisoimage` (`sudo apt install genisoimage`), avec `scramble` et `makeip` de KOS (`make -C $KOS_BASE/utils`), pour l'image ISO (DreamShell).

## Organisation des dossiers

Le script de construction attend cette organisation (les noms sont modifiables, voir plus bas) :

```
Blazing Dragons DC/            dossier de travail (les images disque y sont écrites)
├── src/                       ce dépôt
├── extracted/                 fichiers de la version française (étape 2)
├── extracted_us/              fichiers de la version américaine
└── hd/
    ├── HD.DAT                 graphismes HD (facultatif)
    └── HDSPR.DAT
```

## Construire le jeu

**1. Récupérer le code**

```sh
mkdir "Blazing Dragons DC" && cd "Blazing Dragons DC"
git clone https://github.com/C-DCPort/BLAZINGDRAGONS-DC src
```

**2. Extraire les fichiers du jeu** à partir de votre ou vos images disque :

```sh
python3 src/tools/extract_iso.py "Blazing Dragons (France).bin" extracted       # version française
python3 src/tools/extract_iso.py "Blazing Dragons (USA).bin" extracted_us       # version américaine
```

Vous pouvez faire l'une ou l'autre, ou les deux :

| Dossiers présents | Disque obtenu |
|---|---|
| `extracted` et `extracted_us` | bilingue, français par défaut |
| `extracted` seul | français |
| `extracted_us` seul | anglais |

Versions de référence : `DRAGON.EXE` de 372 736 octets (France) ou 368 640 octets (USA). Pour la version française,
`BIGFILE.DAT` fait 45 107 200 octets, avec le MD5 des 5000 premiers octets égal à `9854fed0d2b48522a62973e99b52a0be`.

**3. Ajouter les graphismes HD** : copier `HD.DAT` et `HDSPR.DAT` dans le dossier `hd/`.

**4. Compiler et créer les images disque**

```sh
cd src && ./build_dc.sh
```

Au premier lancement, le script télécharge les sources nécessaires de ScummVM. Ensuite, il crée dans le dossier de travail :

| Fichier | Pour |
|---|---|
| `Blazing Dragons.cdi` | Flycast, GDEMU, CD gravé |
| `Blazing Dragons.iso` | DreamShell (isoldr) |

Le détail de la compilation est écrit dans `src/build_dc.log` et `src/make.log`.

**Mettre à jour** :

```sh
cd src && git pull && ./build_dc.sh
```

### Options

| Commande | Effet |
|---|---|
| `./build_dc.sh clean` | recompile tout |
| `./build_dc.sh nolog` | aucune sortie texte (pour jouer sur console) |
| `OUT=… ./build_dc.sh` | autre dossier de sortie (par défaut : le dossier parent de `src`) |
| `DATA=… ./build_dc.sh` | autre dossier pour la version française (par défaut : `extracted`) |
| `DATA_US=… ./build_dc.sh` | autre dossier pour la version américaine (par défaut : `extracted_us`) |
| `NO_US=1 ./build_dc.sh` | disque en français seulement, même si `extracted_us` existe |

## En jeu

- **Graphismes** : Options > Graphismes (HD ou originaux, si les fichiers HD sont présents).
- **Langue** : Options > Langue, sur un disque bilingue (le jeu redémarre).
- Les sauvegardes vont sur la première VMU trouvée (port A, puis B, C, D).

## Licence

Code sous GPL v3 ou ultérieure (dérivé de ScummVM). KallistiOS est sous licence de type BSD.
*Blazing Dragons* © ses ayants droit respectifs.
