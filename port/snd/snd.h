/* Son du portage : mélangeur logiciel de type SPU PlayStation (ADPCM VAG + ADSR),
   séquenceur SEQ et voix XA. Indépendant de la plate-forme : le PC écrit un WAV,
   la Dreamcast envoie le flux à l'AICA (port/dc/audio_dc.cpp). */
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace Snd {

typedef uint8_t u8; typedef int8_t s8; typedef uint16_t u16; typedef int16_t s16;
typedef uint32_t u32; typedef int32_t s32; typedef int64_t s64; typedef uint64_t u64;

static const int OUT_RATE = 44100;     /* fréquence de sortie du mélangeur : celle du SPU PlayStation (les échantillons y sont à leur hauteur d'origine) */
static const int MAX_VOICES = 24;

/* ---------- Voix materielles (Dreamcast : canaux AICA) ----------
   Le melangeur garde toute la logique (voix, enveloppes ADSR, gains) ; seul le rendu des voix VAG change : l'echantillon est
   decode en PCM 16 bits dans la RAM de l'AICA et chaque voix y joue sur un canal, volume/panoramique/hauteur mis a jour
   par Mixer::hwService(). Une voix dont l'echantillon ne peut pas etre charge reste mixee en logiciel. */
struct HwSample { u32 addr; u32 samples; u32 loopStart; bool loop; };
struct HwOps {
	bool (*upload)(const u8 *vag, size_t size, HwSample *out);                        /* decode + charge (cache par pointeur) */
	void (*play)(int chan, const HwSample &s, int freqHz, int vol, int pan);          /* vol 0..255, pan 0..255 (128 = centre) */
	void (*update)(int chan, int freqHz, int vol, int pan);                           /* freqHz/vol/pan < 0 : inchange */
	void (*stop)(int chan);
	void (*invalidate)(const u8 *base, size_t size);                                  /* la banque va etre liberee */
	int chanBase;                                                                     /* canal AICA de la voix 0 ; voix i -> chanBase + i */
};
void setHwOps(const HwOps *ops);
/* Decode un VAG en PCM 16 bits (suit les drapeaux de boucle comme le SPU). Renvoie le nombre d'echantillons (0 si trop long). */
u32 vagToPcm(const u8 *vag, size_t size, s16 *out, u32 maxSamples, u32 *loopStart, bool *loop);

void smoothPcm(s16 *p, u32 n, u32 loopStart, bool loop);   /* pre-filtre avant l'envoi a l'AICA */

/* Verrou entre le thread du jeu et le thread audio (nul sur PC). */
struct Hooks { void (*lock)(); void (*unlock)(); };
void setHooks(const Hooks &h);

/* ---------- Banque VAB (en-tête + corps ADPCM) ---------- */
struct Tone {
	u8 vol, pan, center, shift, minKey, maxKey;
	u16 adsr1, adsr2;
	int vag;            /* index 0.. dans la banque, -1 si absent */
};
class VabBank {
public:
	VabBank() : _body(nullptr), _bodySize(0), _numProg(0), _numVag(0), _masterVol(127), _tones(nullptr) { for (int i = 0; i < 128; i++) { _progTones[i] = 0; _progVol[i] = 127; } }
	~VabBank();
	/* vh : en-tête seul ; vb : corps. Les tampons sont copiés (copie du corps pour ne pas garder l'archive). */
	bool load(const u8 *vh, size_t vhSize, const u8 *vb, size_t vbSize);
	bool loadCombined(const u8 *data, size_t size);   /* .MSF : en-tête puis corps */
	/* Remplit out[] avec les tons du programme dont la plage couvre key. Renvoie le nombre. */
	int find(int program, int key, const Tone **out, int maxOut) const;
	const u8 *vag(int index, size_t *size) const;
	int masterVol() const { return _masterVol; }
	int programVol(int p) const { return (p >= 0 && p < 128) ? _progVol[p] : 127; }
private:
	bool parseHeader(const u8 *vh, size_t vhSize, size_t *headerEnd);
	u8 *_body; size_t _bodySize;
	int _numProg, _numVag, _masterVol;
	u8 _progTones[128], _progVol[128];
	Tone *_tones;            /* numProg*16 */
	u32 _vagOff[257], _vagSize[257];
};

/* ---------- Voix ---------- */
struct Voice {
	bool active;
	int tag;                 /* identifiant pour arrêt (numéro de son) */
	int group;               /* 0 musique, 1 effets */
	u32 age;
	u32 pos, step, baseStep; /* 16.16 */
	s32 gainL, gainR;        /* 0..4096 (hors volume de groupe) */
	s32 baseGain, chanGain;  /* Q12 */
	int tonePan, chanPan;
	/* VAG */
	const u8 *data; size_t numBlocks; size_t blk;
	s16 buf[3 + 56];         /* 3 echantillons d'historique (interpolation 4 points) + bloc courant + bloc suivant */
	s32 s1, s2;
	size_t loopBlk; s32 loopS1, loopS2;
	bool dataEnded, curLast, nextLast;
	/* enveloppe */
	bool useEnv; int phase;  /* 0 attaque 1 déclin 2 maintien 3 relâche 4 fini */
	s32 level; s32 cycles;
	u16 adsr1, adsr2;
	u32 tickAcc;
	bool pedalHeld, pendingOff;
	int chan; int key;       /* musique */
	/* rendu materiel */
	bool hw, hwPending; HwSample hwS; u64 hwPos; int hwVol, hwPan, hwFreq;
};

struct XaVoice {
	bool active;
	const u8 *data; int sectors; int sec;      /* données brutes : 2304 octets par secteur */
	bool stereo; int framesPerSec;
	u32 pos, step;                              /* 16.16 en trames dans cur */
	s16 cur[4032], next[4032];
	s32 st[2][2];                               /* état ADPCM de décodage */
	s32 gainL, gainR;
	s16 hist[2][3];                             /* 3 derniers echantillons du secteur precedent (par canal) */
	s16 lastOut[2]; int fadeIn;                 /* derniere sortie (decroissance en fin) et fondu d entree */
	bool up2; bool ended; int padFrames;        /* mode externe : source 18,9 kHz sur-echantillonnee x2 ; fin des donnees ; silence envoye apres la fin */
};

/* Flux PCM continu (son des vidéos STR) : un anneau de trames stéréo décodées depuis des secteurs XA. */
struct StreamVoice {
	bool active, started, ended, starving;
	s32 lastL, lastR; int fade;      /* dernier echantillon (decroit pendant une panne) et rampe d'entree apres une panne */
	s16 *ring;                       /* STREAM_FRAMES trames stéréo entrelacées */
	u32 wr, rd;                      /* compteurs absolus de trames */
	u32 pos, step;                   /* 16.16 : position fractionnaire de lecture, pas en trames source par trame de sortie */
	s32 st[2][2];                    /* état ADPCM entre secteurs */
	s32 gain;
};
static const int STREAM_FRAMES = 32768;

class Sequencer;

class Mixer {
public:
	static Mixer &get();
	/* Gain supplémentaire des répliques (Q12, 4096 = 1) : la version anglaise est enregistrée ~3 dB plus bas que la française */
	static int speechBoost;
	/* Thread audio. Prend le verrou lui-même. */
	void render(s16 *stereoOut, int frames);
	/* Tout le reste : à appeler sous Snd::Lock (le séquenceur l'est déjà). */
	void setVolumes(int music, int sfx, int speech) { _musicVol = music; _sfxVol = sfx; _speechVol = speech; }
	void setDuck(bool on) { _duck = on; }

	int startVag(const u8 *data, size_t size, int tag, int group, int key, int center,
	             int baseGainQ12, int tonePan, int chanGainQ12, int chanPan,
	             bool env, u16 adsr1, u16 adsr2, int chan);
	void stopTag(int tag, int group);
	bool isTagActive(int tag, int group);
	void stopGroup(int group);
	void noteOff(int chan, int key, bool pedal);
	void releaseChan(int chan);
	void pedalRelease(int chan);
	void updateChan(int chan, int chanGainQ12, int chanPan);
	void setChanBend(int chan, float semis);

	void startSpeech(const u8 *data, int sectors, bool stereo, bool rate18900, int gainQ12);
	bool speechActive() { return _xa.active; }
	void stopSpeech() { _xa.active = false; if (_xaExternal) _spEvent = 2; }
	/* Mode externe (Dreamcast) : la parole est un flux separe joue par l'AICA, a 37,8 kHz (18,9 kHz sur-echantillonne x2) */
	void setSpeechExternal(bool on, int latencyFrames) { _xaExternal = on; _spLatency = latencyFrames; }
	int takeSpeechEvent() { int e = _spEvent; _spEvent = 0; return e; }   /* 1 = demarrer, 2 = arreter ; appeler sous Lock */
	bool speechStereo() const { return _xa.stereo; }
	bool speechEnded() const { return _xa.ended; }                         /* toutes les donnees ont ete envoyees au flux */
	int speechRealFrames() const { return _xa.sectors * _xa.framesPerSec * (_xa.up2 ? 2 : 1); }   /* trames a 37,8 kHz */
	void finishSpeech() { _xa.active = false; }
	int renderSpeechNative(s16 *out, int frames);                          /* entrelace x.stereo ? 2 : 1 canaux */

	/* Flux vidéo : streamBegin ; streamPushXa pour chaque secteur audio ; streamEnd (laisse finir le tampon) ou streamStop. */
	bool streamBegin();
	void streamPushXa(const u8 *xa2304, bool stereo, bool rate18900);
	void streamEnd() { Lock2(); }
	void streamStop();
	int streamQueued();
	bool streamStarted() { return _str.active && _str.started; }
	int streamLagMs() { int hz = (int)(((u64)_str.step * 44100) >> 16); return hz > 0 ? (int)((_str.wr - _str.rd) * 1000ULL / hz) : 0; }   /* duree du son en attente dans l'anneau */              /* trames en attente */
	bool streamActive() { return _str.active; }

	/* Mode materiel : a appeler regulierement depuis le thread audio avec le nombre de trames ecoulees (temps reel). */
	void hwService(int frames);
	void setSequencer(Sequencer *s) { _seq = s; }
	int activeVoices();

	Voice _v[MAX_VOICES];
	bool _hwLive[MAX_VOICES];
	XaVoice _xa;
	bool _xaExternal = false; int _spLatency = 0; volatile int _spEvent = 0;
	StreamVoice _str;
	int _musicVol, _sfxVol, _speechVol; bool _duck;
	Sequencer *_seq;
	float _bend[16];
	u32 _age;
	u32 statSpeech, statSfx, statNotes, statClip, statStrUnder;   /* compteurs de diagnostic (statClip : echantillons qui auraient sature) */
private:
	Mixer();
	void mixVoice(Voice &v, s32 *acc, int frames, s32 groupGain);
	void hwCompute(Voice &v, s32 groupGain, int *freq, int *vol, int *pan);
	void mixXa(s32 *acc, int frames);
	void mixStream(s32 *acc, int frames);
	void Lock2();
	int allocVoice(int group);
};

struct Lock { Lock(); ~Lock(); };

/* ---------- Séquenceur SEQ ---------- */
class Sequencer {
public:
	Sequencer(const VabBank *bank) : _bank(bank), _data(nullptr), _playing(false) {}
	~Sequencer();
	bool start(const u8 *seq, size_t size);   /* copie les données */
	void stop();
	bool playing() const { return _playing; }
	void advance(int frames);                 /* appelé par le mélangeur (verrou déjà pris) */
private:
	void event();
	void noteOn(int ch, int key, int vel);
	void noteOff(int ch, int key);
	void restart();
	u32 readVar();
	const VabBank *_bank;
	u8 *_data; size_t _size, _pos, _loopPos; bool _hasLoopPos; size_t _loopEndMark;
	bool _playing; int _ppqn; u32 _tempo;
	s64 _framesPerTick16;   /* 16.16 */
	s64 _acc16;             /* frames restantes avant le prochain événement, 16.16 */
	u8 _status;
	u8 _prog[16], _vol[16], _pan[16], _expr[16]; u8 _sustain[16];
	bool _heldKeys[16][128];
};

}  // namespace Snd
