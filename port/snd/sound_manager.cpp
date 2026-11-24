/* SoundManager du moteur Dragons, version portage : le mélangeur et le séquenceur sont dans snd.cpp.
   Reprend la logique de engines/dragons/sound.cpp (volumes des effets, identifiants de sons, musique par scène). */
#define FORBIDDEN_SYMBOL_ALLOW_ALL
#include "common/scummsys.h"
#include "common/config-manager.h"
#include "common/file.h"
#include "dragons/dragons.h"
#include "dragons/sound.h"
#include "dragons/bigfile.h"
#include "dragons/dragonrms.h"
#include "dragons/strplayer.h"
#include "snd.h"
#include "disc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

namespace Dragons {

struct SoundManager::Impl {
	Snd::VabBank musx, glob, msf;
	bool haveMsf = false;
	Snd::Sequencer *seq = nullptr;
	uint8 speechVolume = 223, sfxVolume = 127, musicVolume = 127;   /* reglages par defaut : 7/8 dialogues, 4/8 effets et musique (retour d'essais sur console) */
	uint8 sfxVolumeTbl[0x780];
	int16 currentSong = -1;
	/* parole */
	struct Entry { uint32 id, off; uint16 sectors; uint8 fmt, pad; };
	Entry *index = nullptr; uint32 indexCount = 0; bool indexTried = false;
	uint8 *clip = nullptr;
	bool legacyTried = false;
};

static void initVolumeTable(uint8 *t);

SoundManager::SoundManager(DragonsEngine *vm, BigfileArchive *b, DragonRMS *r)
	: _dat_8006bb60_sound_related(0), _vm(vm), _bigFileArchive(b), _dragonRMS(r), _impl(new Impl) {
	initVolumeTable(_impl->sfxVolumeTbl);
	uint32 hs, bs;
	{
		byte *h = _bigFileArchive->load("musx.vh", hs), *d = _bigFileArchive->load("musx.vb", bs);
		if (!_impl->musx.load(h, hs, d, bs)) warning("musx.vab invalide");
		free(h); free(d);
	}
	{
		byte *h = _bigFileArchive->load("glob.vh", hs), *d = _bigFileArchive->load("glob.vb", bs);
		if (!_impl->glob.load(h, hs, d, bs)) warning("glob.vab invalide");
		free(h); free(d);
	}
	_impl->seq = new Snd::Sequencer(&_impl->musx);
	{
		Snd::Lock lk;
		Snd::Mixer::get().setSequencer(_impl->seq);
	}
	syncSoundSettings();
}

SoundManager::~SoundManager() {
	{
		Snd::Lock lk;
		Snd::Mixer::get().setSequencer(nullptr);
		Snd::Mixer::get().stopSpeech();
		Snd::Mixer::get().stopGroup(0);
		Snd::Mixer::get().stopGroup(1);
	}
	delete _impl->seq;
	free(_impl->clip);
	free(_impl->index);
	delete _impl;
}

void SoundManager::syncSoundSettings() {
	if (ConfMan.hasKey("music_volume")) _impl->musicVolume = CLIP<int>(ConfMan.getInt("music_volume"), 0, 255);
	if (ConfMan.hasKey("sfx_volume")) _impl->sfxVolume = CLIP<int>(ConfMan.getInt("sfx_volume"), 0, 255);
	if (ConfMan.hasKey("speech_volume")) _impl->speechVolume = CLIP<int>(ConfMan.getInt("speech_volume"), 0, 255);
	Snd::Lock lk;
	Snd::Mixer::get().setVolumes(_impl->musicVolume, _impl->sfxVolume, _impl->speechVolume);
}

/* ---- effets ---- */
void SoundManager::playOrStopSound(uint16 soundId) {
	uint16 volumeId;
	if ((soundId & 0x8000u) == 0) volumeId = (soundId & ~0x4000u) + _vm->getCurrentSceneId() * 0x20;
	else volumeId = soundId & ~(0x4000u | 0x8000u);
	if (volumeId >= sizeof(_impl->sfxVolumeTbl)) return;

	if ((soundId & 0x4000u) == 0) {
		byte volume = _impl->sfxVolumeTbl[volumeId] & 0x1fu;
		_impl->sfxVolumeTbl[volumeId] |= 0x40u;
		bool global = (soundId & 0x8000u) != 0;
		Snd::VabBank &bank = global ? _impl->glob : _impl->msf;
		if (!global && !_impl->haveMsf) return;
		uint16 realId = soundId & 0x7fffu;
		int program = realId >> 4u;
		int key = ((realId & 0xfu) << 1u) | 0x40u;
		Snd::Lock lk;
		Snd::Mixer &m = Snd::Mixer::get();
		m.stopTag(soundId, 1);
		const Snd::Tone *tones[8];
		int n = bank.find(program, key, tones, 8);
		if (!n) { debug(3, "Son introuvable programme %d touche %d", program, key); return; }
		for (int i = 0; i < n; i++) {
			size_t sz; const uint8 *d = bank.vag(tones[i]->vag - 1, &sz);
			int gain = 4096 * volume / 31 * tones[i]->vol / 127;
			m.startVag(d, sz, soundId, 1, key, tones[i]->center, gain, 64, 4096, 64, false, 0, 0, -1);
		}
	} else {
		_impl->sfxVolumeTbl[volumeId] &= 0xbfu;
		Snd::Lock lk;
		Snd::Mixer::get().stopTag(soundId & ~0x4000u, 1);
	}
}

void SoundManager::loadMsf(uint32 sceneId) {
	char name[] = "XXXX.MSF";
	memcpy(name, _dragonRMS->getSceneName(sceneId), 4);
	if (_bigFileArchive->doesFileExist(name)) {
		uint32 size;
		byte *data = _bigFileArchive->load(name, size);
		{
			Snd::Lock lk;
			Snd::Mixer::get().stopGroup(1);
			_impl->haveMsf = _impl->msf.loadCombined(data, size);
		}
		free(data);
	}
}

/* ---- musique ---- */
void SoundManager::playMusic(int16 song) {
	if (_impl->currentSong == song) return;
	_impl->currentSong = song;
	Common::String filename = Common::String(_vm->_dragonRMS->getSceneName(_vm->getCurrentSceneId()), 4);
	filename += Common::String::format("z%02d.msq", song);
	if (!_bigFileArchive->doesFileExist(filename.c_str())) { warning("Musique introuvable %s", filename.c_str()); return; }
	uint32 size;
	byte *data = _bigFileArchive->load(filename.c_str(), size);
	{
		Snd::Lock lk;
		if (!_impl->seq->start(data, size)) warning("Musique invalide %s", filename.c_str());
	}
	free(data);
}

/* ---- parole ---- */
static bool loadIndex(SoundManager::Impl *s) {
	if (s->indexTried) return s->index != nullptr;
	s->indexTried = true;
	Common::File f;
	if (!f.open("speech.pak")) return false;
	byte hdr[8];
	if (f.read(hdr, 8) != 8 || memcmp(hdr, "SPCH", 4) != 0) return false;
	uint32 n = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | ((uint32)hdr[7] << 24);
	if (n == 0 || n > 100000) return false;
	byte *raw = (byte *)malloc(n * 12);
	if (!raw) return false;
	if (f.read(raw, n * 12) != n * 12) { free(raw); return false; }
	s->index = (SoundManager::Impl::Entry *)malloc(n * sizeof(SoundManager::Impl::Entry));
	for (uint32 i = 0; i < n; i++) {
		const byte *r = raw + i * 12;
		s->index[i].id = r[0] | (r[1] << 8) | (r[2] << 16) | ((uint32)r[3] << 24);
		s->index[i].off = r[4] | (r[5] << 8) | (r[6] << 16) | ((uint32)r[7] << 24);
		s->index[i].sectors = r[8] | (r[9] << 8);
		s->index[i].fmt = r[10]; s->index[i].pad = 0;
	}
	free(raw);
	s->indexCount = n;
	return true;
}

void SoundManager::playSpeech(uint32 textIndex) {
	Impl *s = _impl;
	{
		Snd::Lock lk;
		Snd::Mixer::get().stopSpeech();
	}
	free(s->clip); s->clip = nullptr;
	uint32 sectors = 0; uint8 fmt = 0;

	if (loadIndex(s)) {
		/* recherche dichotomique (table triée par identifiant) */
		int lo = 0, hi = (int)s->indexCount - 1, found = -1;
		while (lo <= hi) {
			int mid = (lo + hi) / 2;
			if (s->index[mid].id == textIndex) { found = mid; break; }
			if (s->index[mid].id < textIndex) lo = mid + 1; else hi = mid - 1;
		}
		if (found < 0) return;
		sectors = s->index[found].sectors; fmt = s->index[found].fmt;
		s->clip = (uint8 *)malloc((size_t)sectors * 2304);
		if (!s->clip) return;
		/* lecture alignée sur les secteurs (une seule commande disque pour toute la réplique) */
		static Disc::File *speechFile = nullptr;
		if (!speechFile) speechFile = Disc::open("speech.pak");
		if (!speechFile || Disc::readAt(speechFile, s->index[found].off, s->clip, sectors * 2304) != (long)(sectors * 2304)) {
			free(s->clip); s->clip = nullptr; return;
		}
	} else {
		/* secours : lecture directe de dtspeech.xa (secteurs entrelacés de 32, lent sur disque) */
		Common::File exe;
		if (!exe.open("dragon.exe")) return;
		exe.seek(_vm->getSpeechTblOffsetFromDragonEXE());
		uint16 start = 0, end = 0; int8 startOffset = 0; bool ok = false;
		for (int i = 0; i < 2272; i++) {
			uint32 id = exe.readUint32LE() & 0xffffff;
			exe.seek(-1, SEEK_CUR);
			startOffset = exe.readSByte(); start = exe.readUint16LE(); end = exe.readUint16LE();
			if (id == textIndex) { ok = true; break; }
		}
		if (!ok || end <= start) return;
		Common::File xa;
		if (!xa.open("dtspeech.xa")) return;
		sectors = end - start;
		s->clip = (uint8 *)malloc((size_t)sectors * 2304);
		if (!s->clip) return;
		byte sec[2352];
		for (uint32 i = 0; i < sectors; i++) {
			xa.seek((int64)(((uint32)start * 32 + startOffset + i * 32)) * 2352);
			if (xa.read(sec, 2352) != 2352) { free(s->clip); s->clip = nullptr; return; }
			if (i == 0) fmt = sec[19];
			memcpy(s->clip + (size_t)i * 2304, sec + 24, 2304);
		}
	}
	bool stereo = (fmt & 1) != 0, r18900 = (fmt & 4) != 0;
	{ static int logged = 0; if (logged < 3) { logged++; printf("BD: replique %u : format XA 0x%02x (%s, %d Hz), %u secteurs\n", textIndex, fmt, stereo ? "stereo" : "mono", r18900 ? 18900 : 37800, sectors); } }
	_vm->setFlags(ENGINE_FLAG_8000);
	Snd::Lock lk;
	Snd::Mixer &m = Snd::Mixer::get();
	m.startSpeech(s->clip, (int)sectors, stereo, r18900, 4096);
	m.setDuck(true);
}

bool SoundManager::isSpeechPlaying() {
	Snd::Lock lk;
	return Snd::Mixer::get().speechActive();
}

int SoundManager::speechDurationMs() {
	Snd::Lock lk;
	return (int)((long long)Snd::Mixer::get().speechRealFrames() * 1000 / 37800);
}

void SoundManager::resumeMusic() {
	Snd::Lock lk;
	Snd::Mixer &m = Snd::Mixer::get();
	if (m.speechActive()) { m.stopSpeech(); _vm->clearFlags(ENGINE_FLAG_8000); }
	m.setDuck(false);
}

bool SoundManager::getSpeechLocation(uint32, struct SpeechLocation *) { return false; }
bool SoundManager::isVoicePlaying(uint16) { return false; }
void SoundManager::stopAllVoices() {}
void SoundManager::playSound(uint16, uint16) {}
void SoundManager::stopSound(uint16, uint16) {}
void SoundManager::SomeInitSound_FUN_8003f64c() {}
void SoundManager::initVabData() {}

static void initVolumeTable(uint8 *t) {
	memset(t, 0x10, 0x780);
	static const struct { int i; uint8 v; bool orMask; } kTbl[] = {
		{192,0x0b,0},{193,0x0b,0},{226,0x80,1},{229,0x0b,0},{230,0x0b,0},{450,0x0b,0},{451,0x0b,0},{514,0x8b,0},{515,0x0b,0},{516,0x0b,0},
		{578,0x0b,0},{579,0x0b,0},{580,0x0b,0},{611,0x0b,0},{674,0x8b,0},{675,0x88,0},{711,0x08,0},{866,0x0b,0},{896,0x0b,0},{897,0x80,1},
		{930,0x80,1},{934,0x8b,0},{935,0x8b,0},{936,0x0b,0},{937,0x88,0},{941,0x0b,0},{964,0x0b,0},{995,0x80,1},{1027,0x08,0},{1056,0x8b,0},
		{1059,0x80,1},{1122,0x0b,0},{1250,0x08,0},{1252,0x0b,0},{1256,0x0b,0},{1257,0x08,0},{1258,0x0b,0},{1284,0x0b,0},{1378,0x0b,0},
		{1379,0x80,1},{1380,0x0b,0},{1385,0x0b,0},{1443,0x8b,0},{1444,0x80,1},{1445,0x80,1},{1446,0x8b,0},{1472,0x8b,0},{1508,0x80,1},
		{1575,0x08,0},{1576,0x08,0},{1577,0x08,0},{1604,0x08,0},{1605,0x08,0},{1610,0x0b,0},{1611,0x0b,0},{1612,0x0b,0},
	};
	for (auto &e : kTbl) { if (e.orMask) t[e.i] |= e.v; else t[e.i] = e.v; }
}

} // namespace Dragons
