#!/bin/bash
# Compile la version Dreamcast depuis WSL. Tout est écrit dans build_dc.log.
#   ./build_dc.sh          compile (composition par le PVR, seule version construite)
#   ./build_dc.sh clean    recompile tout
cd "$(dirname "$0")"
exec > >(tee build_dc.log) 2>&1
echo "=== $(date)"
for f in /opt/toolchains/dc/kos/environ.sh "$KOS_BASE/environ.sh"; do
	[ -f "$f" ] && { source "$f"; echo "environ : $f"; break; }
done
[ -z "$KOS_BASE" ] && { echo "KOS_BASE introuvable : indiquez le chemin de environ.sh"; exit 1; }
echo "KOS_BASE=$KOS_BASE"; kos-c++ --version | head -1
( cd "$KOS_BASE" && git log -1 --format='KOS %h %cd' 2>/dev/null )
ls -d "$KOS_BASE"/../kos-ports/libGL "$KOS_BASE"/addons/lib/dreamcast/libGL* 2>/dev/null

# Sources de ScummVM (conteneurs, flux, surfaces), au commit noté dans engine/UPSTREAM
if [ ! -d scummvm/common ]; then
	git clone --filter=blob:none --sparse https://github.com/scummvm/scummvm scummvm &&
	( cd scummvm && git sparse-checkout set --no-cone /common/ /graphics/ /math/ &&
	  git checkout -q "$(cut -d' ' -f2 ../engine/UPSTREAM)" )
fi

# La composition par le PVR est le seul mode construit (l'argument "gpu" est accepté et ignoré).
GPU_FLAG="GPU=1"; SUFFIX=""
# Par defaut : musique et effets joues par les canaux AICA (valide sur console). "sw" : ancien melangeur logiciel (fichiers "Blazing Dragons SW").
HW_FLAG="HW=1"; LOG_FLAG=""; NOLOG_FLAG=""; PERF_FLAG=""
# "nolog" : aucune sortie texte (console série / journal Flycast coupés), pour jouer sur vraie console
# "log" : statistiques periodiques sur la sortie serie (par defaut desactivees : chaque bloc de texte bloque le jeu ~100 ms)
for a in "$@"; do
	[ "$a" = log ] && LOG_FLAG="LOG=1"
	[ "$a" = perf ] && echo "perf : remplacé par l'option « Jauges » du menu Options"
	[ "$a" = nolog ] && { NOLOG_FLAG="NOLOG=1"; LOG_FLAG=""; }
	[ "$a" = clean ] && make -C port/dc clean
	[ "$a" = sw ] && { HW_FLAG=""; SUFFIX=" SW"; }
done
[ -n "$HW_FLAG" ] && echo "audio : voix materielles AICA" || echo "audio : melangeur logiciel"
# audio_dc.cpp depend du mode : toujours recompile
touch port/dc/audio_dc.cpp port/dc/main_dc.cpp
echo "mode : composition par le PVR (GPU)"
# un changement de mode doit tout recompiler les fichiers qui en dépendent
touch port/dc/main_dc.cpp port/common/gpu_core.cpp
# L'ancien bd.elf ne doit jamais être réutilisé si la compilation échoue.
rm -f port/dc/bd.elf
export BD_BUILD="$(git rev-parse --short HEAD 2>/dev/null)$(git diff --quiet 2>/dev/null || echo '+modif')_$(date +%H%M%S)"
BD_BUILD="${BD_BUILD}${SUFFIX// /_}"; export BD_BUILD
echo "version : $BD_BUILD"
[ -n "$NOLOG_FLAG" ] && echo "journal : desactive (nolog)"
touch port/dc/main_dc.cpp   # pour que la version (BD_BUILD) soit toujours recompilée
make -C port/dc -j"$(nproc)" -k $GPU_FLAG $HW_FLAG $LOG_FLAG $NOLOG_FLAG $PERF_FLAG > make.log 2>&1
MAKE_RC=$?
grep -E 'error|Error|warning: undefined|undefined reference|^kos-c\+\+ -o' make.log | head -80
[ $MAKE_RC -ne 0 ] && { echo "ERREUR : la compilation a échoué (voir make.log), aucune image disque créée"; exit 1; }
ls -la port/dc/bd.elf 2>/dev/null && sh-elf-size port/dc/bd.elf 2>/dev/null
[ -f port/dc/bd.elf ] || { echo "ERREUR : bd.elf absent, pas d'image disque"; exit 1; }

# ---- Images disque : CDI (Flycast / GDEMU) et ISO plate (DreamShell / isoldr) ----
# Dossier de sortie : le parent du dépôt (E:\DC_GDI\Blazing Dragons DC), modifiable par OUT=
# Données du jeu : <sortie>/extracted (créé par tools/extract_iso.py), modifiable par DATA=
ROOT=$PWD
OUT=${OUT:-$(cd .. && pwd)}
DATA=${DATA:-$OUT/extracted}
DATA_US=${DATA_US:-$OUT/extracted_us}
# Version anglaise seule : pas de version française dans $DATA, mais une version américaine dans $DATA_US
if [ ! -f "$DATA/DRAGON.EXE" ] && [ ! -f "$DATA/dragon.exe" ] && [ -f "$DATA_US/DRAGON.EXE" ]; then
	echo "version française absente : disque en anglais seulement"
	DATA=$DATA_US; NO_US=1
fi
STAGE=${STAGE:-$HOME/bd-build}          # sur le disque Linux de WSL : plus rapide que /mnt/e
NAME="Blazing Dragons${SUFFIX}"
echo "sortie : $OUT"; echo "données : $DATA"
for f in BIGFILE.DAT DRAGON.EXE DTSPEECH.XA CRYSTALD.STR ILLUSION.STR LABINTRO.STR PREVIEWS.STR; do
	[ -f "$DATA/$f" ] || [ -f "$DATA/$(echo $f | tr A-Z a-z)" ] || { echo "ERREUR : $f introuvable dans $DATA"; exit 1; }
done
# dossier du disque : données du jeu uniquement (ni MANIFEST ni autres fichiers)
rm -rf "$STAGE/disc"; mkdir -p "$STAGE/disc"
for f in "$DATA"/*; do
	case "$(basename "$f" | tr a-z A-Z)" in
		BIGFILE.DAT|DRAGON.EXE|DTSPEECH.XA|ARC[0-9].BIN|*.STR) cp -u "$f" "$STAGE/disc/$(basename "$f" | tr a-z A-Z)";;
	esac
done
# Graphismes HD (optionnels) : HD.DAT (décors) et HDSPR.DAT (sprites), pris d'abord dans $OUT/hd (là où arrivent les
# nouvelles versions), sinon dans $DATA ; sans eux le menu reste sur "Originaux". Copie dès que le fichier diffère
# (cp -u gardait une ancienne copie de $DATA prioritaire).
for f in HD.DAT HDSPR.DAT; do
	for d in "$OUT/hd" "$DATA"; do
		if [ -f "$d/$f" ]; then
			cmp -s "$d/$f" "$STAGE/disc/$f" 2>/dev/null || cp "$d/$f" "$STAGE/disc/$f"
			echo "HD : $f ajouté au disque depuis $d ($(( $(stat -c %s "$d/$f") / 1048576 )) Mo)"
			[ "$d" = "$DATA" ] || { [ -f "$DATA/$f" ] && echo "     (ancienne copie ignorée : $DATA/$f)"; }
			break
		fi
	done
done
# Sous-titres des videos : subs/*.sub (dans le depot ; textes, pas des donnees du jeu) puis eventuellement ceux de $DATA
for f in "$ROOT"/subs/*.sub "$DATA"/*.sub "$DATA"/*.SUB; do
	[ -f "$f" ] && cp -u "$f" "$STAGE/disc/$(basename "$f" | tr a-z A-Z)"
done
# La parole : DTSPEECH.XA (entrelacé, lent à lire) -> SPEECH.PAK (répliques contiguës, plus petit)
if [ -f "$STAGE/disc/DTSPEECH.XA" ]; then
	if python3 "$ROOT/tools/xa_pack.py" "$STAGE/disc/DTSPEECH.XA" "$STAGE/disc/DRAGON.EXE" "$STAGE/disc/SPEECH.PAK" ${SPEECH_TABLE:+--table $SPEECH_TABLE}; then
		rm -f "$STAGE/disc/DTSPEECH.XA"
	else
		echo "AVERTISSEMENT : SPEECH.PAK non créé, DTSPEECH.XA conservé (lecture lente)"; rm -f "$STAGE/disc/SPEECH.PAK"
	fi
fi
# Fautes d'orthographe des dialogues français corrigées (version française seulement : DRAGON.EXE de 372 736 octets)
if [ -f "$STAGE/disc/BIGFILE.DAT" ] && [ "$(stat -c %s "$STAGE/disc/DRAGON.EXE" 2>/dev/null)" = 372736 ]; then
	python3 "$ROOT/tools/text_fix.py" "$STAGE/disc/BIGFILE.DAT" | tail -1
fi
# Les ressources : BIGFILE.DAT -> BIGPACK.DAT (compressé ~3x, donc 3x moins d'octets à lire sur le disque)
if [ -f "$STAGE/disc/BIGFILE.DAT" ]; then
	# annuaire des ressources dans DRAGON.EXE : version américaine (368 640 octets) ou française (par défaut)
	[ -z "$BIGFILE_TABLE" ] && [ "$(stat -c %s "$STAGE/disc/DRAGON.EXE")" = 368640 ] && BIGFILE_TABLE=0x4a238 && BIGFILE_N=576
	if python3 "$ROOT/tools/bigfile_pack.py" "$STAGE/disc/DRAGON.EXE" "$STAGE/disc/BIGFILE.DAT" "$STAGE/disc/BIGPACK.DAT" ${BIGFILE_TABLE:+--table $BIGFILE_TABLE} ${BIGFILE_N:+--n $BIGFILE_N}; then
		rm -f "$STAGE/disc/BIGFILE.DAT"
	else
		echo "AVERTISSEMENT : BIGPACK.DAT non créé, BIGFILE.DAT conservé (chargements lents)"; rm -f "$STAGE/disc/BIGPACK.DAT"
	fi
fi
# Disque bilingue : version anglaise dans disc/US (DRAGON.EXE, ressources, voix, 3e intro, ARC*.BIN), si <sortie>/extracted_us
# existe ; vidéos communes et graphismes HD restent dans disc. Choix de la langue : Options > Langue (redémarre le jeu).
if [ -f "$DATA_US/DRAGON.EXE" ] && [ -z "$NO_US" ]; then
	mkdir -p "$STAGE/disc/US"
	for f in "$DATA_US"/*; do
		case "$(basename "$f" | tr a-z A-Z)" in
			DRAGON.EXE|ARC[0-9].BIN|LABINTRO.STR) cp -u "$f" "$STAGE/disc/US/$(basename "$f" | tr a-z A-Z)";;
		esac
	done
	[ -f "$STAGE/disc/US/SPEECH.PAK" ] && [ "$STAGE/disc/US/SPEECH.PAK" -nt "$DATA_US/DTSPEECH.XA" ] ||
		python3 "$ROOT/tools/xa_pack.py" "$DATA_US/DTSPEECH.XA" "$DATA_US/DRAGON.EXE" "$STAGE/disc/US/SPEECH.PAK" --table 0x4e138
	[ -f "$STAGE/disc/US/BIGPACK.DAT" ] && [ "$STAGE/disc/US/BIGPACK.DAT" -nt "$DATA_US/BIGFILE.DAT" ] ||
		python3 "$ROOT/tools/bigfile_pack.py" "$DATA_US/DRAGON.EXE" "$DATA_US/BIGFILE.DAT" "$STAGE/disc/US/BIGPACK.DAT" --table 0x4a238 --n 576
	echo "version anglaise ajoutée (disc/US) : $(du -sh "$STAGE/disc/US" | cut -f1)"
else
	rm -rf "$STAGE/disc/US"
fi
# Programme non brouillé pour la relance après un changement de langue (arch_exec ; le redémarrage par le BIOS
# ne passe pas le logo Dreamcast avec DreamShell)
sh-elf-objcopy -R .stack -O binary port/dc/bd.elf "$STAGE/disc/BD.BIN" || echo "AVERTISSEMENT : BD.BIN non créé (changement de langue par redémarrage complet)"
du -sh "$STAGE/disc"

# CDI
if command -v mkdcdisc >/dev/null; then
	rm -f "$STAGE/bd.cdi"
	mkdcdisc -e "$ROOT/port/dc/bd.elf" -d "$STAGE/disc" -N -n "$NAME" -o "$STAGE/bd.cdi" &&
		cp -v "$STAGE/bd.cdi" "$OUT/$NAME.cdi"
else
	echo "mkdcdisc introuvable : CDI non créé (à installer : voir kos-ports / github.com/KallistiOS/mkdcdisc)"
fi

# ISO pour DreamShell / isoldr : 1ST_READ.BIN brouillé + IP.BIN + données, à la racine
find_tool() { command -v "$1" || find "$KOS_BASE/utils" "$KOS_BASE/../kos-ports" -type f -name "$1" -perm -u+x 2>/dev/null | head -1; }
SCRAMBLE=$(find_tool scramble); MAKEIP=$(find_tool makeip); MKISO=$(command -v mkisofs || command -v genisoimage)
if [ -n "$SCRAMBLE" ] && [ -n "$MAKEIP" ] && [ -n "$MKISO" ]; then
	rm -rf "$STAGE/iso"; mkdir -p "$STAGE/iso"
	cp -a "$STAGE/disc/." "$STAGE/iso/"
	sh-elf-objcopy -R .stack -O binary port/dc/bd.elf "$STAGE/bd.bin" &&
	"$SCRAMBLE" "$STAGE/bd.bin" "$STAGE/iso/1ST_READ.BIN" &&
	rm -f "$STAGE/IP.BIN" "$STAGE/bd.iso" "$OUT/$NAME.iso" &&
	( cd "$STAGE/iso" && "$MAKEIP" -n T-00000 -e V1.000 -a JUE -p E000F10 -g "BLAZING DRAGONS" -c SEGA -b 1ST_READ.BIN "$STAGE/IP.BIN" ) &&
	"$MKISO" -f -V BLAZING_DRAGONS -G "$STAGE/IP.BIN" -r -J -l -o "$STAGE/bd.iso" "$STAGE/iso" &&
	cp -v "$STAGE/bd.iso" "$OUT/$NAME.iso" || echo "ERREUR : ISO non créée (voir les messages ci-dessus)"
else
	echo "ISO non créée : scramble='$SCRAMBLE' makeip='$MAKEIP' mkisofs='$MKISO'"
	echo "  (sudo apt install genisoimage ; scramble et makeip viennent de KOS : make -C \$KOS_BASE/utils)"
fi
ls -la "$OUT"/*.cdi "$OUT"/*.iso 2>/dev/null
echo "=== fin"
