// Décodeur de vidéos PlayStation (.STR) : secteurs CD bruts 2352 octets, images MDEC (v2/v3) et son XA.
// Autonome (pas de dépendance au moteur) ; le format suit la description jpsxdec et le décodeur de ScummVM (GPL).
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Disc { struct File; }

namespace Str {

/* Reçoit un secteur audio XA brut (2304 octets utiles) dans l'ordre du flux. */
typedef void (*AudioCb)(const uint8_t *xa2304, bool stereo, bool rate18900, void *user);

class Decoder {
public:
	Decoder();
	~Decoder();
	bool open(const char *name);            /* nom du fichier dans le dossier des données */
	void close();
	bool isOpen() const { return _file != nullptr; }
	int width() const { return _w; }        /* connus après le premier nextFrame() */
	int height() const { return _h; }
	void setAudio(AudioCb cb, void *user) { _cb = cb; _user = user; }
	/* Lit jusqu'à la fin de la prochaine image et la décode en RGB555 (R bits 10-14, G 5-9, B 0-4).
	   rgb : au moins pitchPx * height pixels (nullptr = saute le décodage, renvoie 1 quand même). Renvoie 1 = image prête, 0 = fin du flux, -1 = erreur. */
	int nextFrame(uint16_t *rgb, int pitchPx);
	uint32_t sectorsRead() const { return _sectors; }     /* secteurs consommés depuis le début (150 par seconde à vitesse x2) */
	uint32_t frameNumber() const { return _frameNo; }
	void setSmooth(bool on) { _smooth = on; }
	bool probeSize(int *w, int *h);         /* lit le début du flux pour connaître la taille sans le consommer */

private:
	Disc::File *_file;
	long _size;
	uint32_t _pos;                          /* octet courant dans le fichier */
	uint8_t *_buf; uint32_t _bufStart, _bufLen;
	uint8_t *_frame; uint32_t _frameCap, _frameLen;
	int _frameChunks, _frameNext;
	uint32_t _frameNo;
	int _w, _h;
	uint32_t _sectors;
	AudioCb _cb; void *_user;
	int _videoChannel;
	uint16_t *_vlc;                         /* table AC (peek 16 bits) */
	uint16_t _vlc8[256];                    /* table AC courte (peek 8 bits, 0 = code plus long) */
	bool _smooth;                           /* chrominance interpolée (plus joli, un peu plus lent) */
	uint16_t *_rowCr, *_rowCb;
	uint8_t *_y, *_cb2, *_cr; int _mbW, _mbH;

	bool readSector(const uint8_t **s);
	bool decodeFrame(uint16_t *rgb, int pitchPx);
};

}
