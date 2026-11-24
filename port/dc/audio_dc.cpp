/* Sortie audio Dreamcast : le mélangeur logiciel (port/snd) alimente un flux AICA (snd_stream).
   Le flux est rempli par un thread qui interroge snd_stream_poll() toutes les quelques ms. */
#include <kos.h>
#include <dc/sound/sound.h>
#include <dc/sound/stream.h>
#include <stdint.h>
#include "snd.h"
#include "audio_dc.h"

static snd_stream_hnd_t g_stream = SND_STREAM_INVALID;
/* Parole : flux AICA separe (37,8 kHz), la musique/les effets restent dans le melangeur logiciel */
static snd_stream_hnd_t g_speech = SND_STREAM_INVALID;
static bool g_speechRunning = false;
static const int kSpeechBuf = 65536;                  /* octets par canal : 32768 echantillons = 867 ms a 37,8 kHz ; marge ~430 ms contre les pauses du jeu (lecture disque) */
static uint64_t g_spT0 = 0, g_spRealMs = 0;           /* heure de depart du flux, duree des donnees reelles */
alignas(32) static int16_t g_spOut[32768 * 2];
static mutex_t g_mutex = MUTEX_INITIALIZER;
static volatile bool g_run = false;
static volatile uint64_t g_lastCb = 0, g_maxGap = 0;
static volatile uint64_t g_msRender = 0, g_framesRendered = 0;
alignas(32) static int16_t g_out[16384 * 2];         /* 16384 trames stéréo max par appel */

static void lockFn() { mutex_lock(&g_mutex); }
static void unlockFn() { mutex_unlock(&g_mutex); }

/* snd_stream demande des OCTETS (stéréo entrelacé 16 bits) */
static void *streamCb(snd_stream_hnd_t, int bytesReq, int *bytesRecv) {
	int frames = bytesReq / 4;
	if (frames > 16384) frames = 16384;
	if (frames < 0) frames = 0;
	uint64_t t0 = timer_ms_gettime64();
	if (g_lastCb && t0 - g_lastCb > g_maxGap) g_maxGap = t0 - g_lastCb;
	g_lastCb = t0;
	Snd::Mixer::get().render(g_out, frames);
	g_msRender += timer_ms_gettime64() - t0; g_framesRendered += frames;
	*bytesRecv = frames * 4;
	return g_out;
}


#ifdef BD_HWAUDIO
/* ---- Voix VAG sur les canaux AICA (voir Snd::HwOps) ----
   Les echantillons sont decodes en PCM 16 bits, lisses (smoothPcm) puis charges dans la RAM de l'AICA (cache par pointeur,
   libere avec la banque). Un echantillon de plus de 65534 trames (limite du pilote KOS) ou sans place reste en logiciel. */
#include <dc/spu.h>
#include <dc/sound/aica_comm.h>
#include <dc/sound/sfxmgr.h>
#include <vector>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

namespace {
struct HwEnt { const uint8_t *p; size_t n; Snd::HwSample s; bool ok; };
std::vector<HwEnt> g_ents;
uint32_t g_chanAddr[64];        /* adresse de l'echantillon joue par chaque canal (pour liberer sans couper un autre) */
unsigned g_hwUploads = 0, g_hwFails = 0, g_hwBytes = 0;

void sendChan(int chn, uint32_t cmd, const Snd::HwSample *smp, int freq, int vol, int pan) {
	AICA_CMDSTR_CHANNEL(tmp, c, ch);
	memset(tmp, 0, sizeof(tmp));
	c->cmd = AICA_CMD_CHAN; c->timestamp = 0; c->size = AICA_CMDSTR_CHANNEL_SIZE; c->cmd_id = chn;
	ch->cmd = cmd;
	if (smp) {
		ch->base = smp->addr; ch->type = AICA_SM_16BIT;
		uint32_t len = smp->samples > 65534 ? 65534 : smp->samples;
		ch->length = len; ch->loop = smp->loop ? 1 : 0;
		ch->loopstart = smp->loop ? smp->loopStart : 0; ch->loopend = len;
	}
	if (freq >= 0) ch->freq = (uint32_t)freq;
	if (vol >= 0) ch->vol = (uint32_t)vol;
	if (pan >= 0) ch->pan = (uint32_t)pan;
	snd_sh4_to_aica(tmp, c->size);
}

bool hwUpload(const uint8_t *vag, size_t size, Snd::HwSample *out) {
	for (auto &e : g_ents) if (e.p == vag && e.n == size) { if (!e.ok) return false; *out = e.s; return true; }
	HwEnt e; e.p = vag; e.n = size; e.ok = false; memset(&e.s, 0, sizeof(e.s));
	uint32_t maxS = (uint32_t)(size / 16) * 28 + 28; if (maxS > 65534 + 28) maxS = 65534 + 28;
	int16_t *pcm = (int16_t *)memalign(32, (size_t)maxS * 2);
	if (pcm) {
		uint32_t ls = 0; bool loop = false;
		uint32_t n = Snd::vagToPcm(vag, size, pcm, maxS, &ls, &loop);
		if (n > 0 && n <= 65534) {
			Snd::smoothPcm(pcm, n, ls, loop);
			if (!loop) { /* effets a lecture unique : debut et fin adoucis (evite le "ting" craquant) */
				uint32_t fi = n < 16 ? n : 16, fo = n < 64 ? n : 64;
				for (uint32_t k = 0; k < fi; k++) pcm[k] = (int16_t)((int)pcm[k] * (int)k / (int)fi);
				for (uint32_t k = 0; k < fo; k++) pcm[n - 1 - k] = (int16_t)((int)pcm[n - 1 - k] * (int)k / (int)fo);
			}
			uint32_t addr = snd_mem_malloc((size_t)n * 2);
			if (addr) {
				spu_memload_sq(addr, pcm, (size_t)n * 2);
				e.s.addr = addr; e.s.samples = n; e.s.loopStart = ls; e.s.loop = loop; e.ok = true;
				g_hwUploads++; g_hwBytes += n * 2;
			}
		}
		free(pcm);
	}
	if (!e.ok) g_hwFails++;
	g_ents.push_back(e);
	if (!e.ok) return false;
	*out = e.s; return true;
}

void hwPlay(int chn, const Snd::HwSample &s, int freq, int vol, int pan) {
	g_chanAddr[chn] = s.addr;
	sendChan(chn, AICA_CH_CMD_START, &s, freq, vol, pan);
}
void hwUpdate(int chn, int freq, int vol, int pan) {
	uint32_t f = AICA_CH_CMD_UPDATE;
	if (freq >= 0) f |= AICA_CH_UPDATE_SET_FREQ;
	if (vol >= 0) f |= AICA_CH_UPDATE_SET_VOL;
	if (pan >= 0) f |= AICA_CH_UPDATE_SET_PAN;
	sendChan(chn, f, nullptr, freq, vol, pan);
}
void hwStop(int chn) { sendChan(chn, AICA_CH_CMD_STOP, nullptr, -1, -1, -1); g_chanAddr[chn] = 0; }

void hwInvalidate(const uint8_t *base, size_t size) {
	for (size_t i = 0; i < g_ents.size();) {
		HwEnt &e = g_ents[i];
		if (e.p >= base && e.p < base + size) {
			if (e.ok) {
				for (int c = 0; c < 64; c++) if (g_chanAddr[c] == e.s.addr) hwStop(c);
				snd_mem_free(e.s.addr); g_hwBytes -= e.s.samples * 2;
			}
			g_ents.erase(g_ents.begin() + i);
		} else i++;
	}
}

Snd::HwOps g_hwOps = { hwUpload, hwPlay, hwUpdate, hwStop, hwInvalidate, 0 };
}
static uint64_t g_hwLastUs = 0, g_hwAccUs = 0;

/* Appele par le thread audio : fait avancer le sequenceur, les enveloppes et les canaux AICA du temps reel ecoule. */
static void hwTick() {
	uint64_t now = timer_us_gettime64();
	if (!g_hwLastUs) { g_hwLastUs = now; return; }
	g_hwAccUs += (now - g_hwLastUs) * 44100; g_hwLastUs = now;
	int frames = (int)(g_hwAccUs / 1000000); g_hwAccUs %= 1000000;
	if (frames > 8820) frames = 8820;     /* apres une tres longue pause : pas de rattrapage au-dela de 200 ms */
	if (frames > 0) Snd::Mixer::get().hwService(frames);
}
static bool hwInit() {
	int first = -1;
	for (int i = 0; i < Snd::MAX_VOICES; i++) {
		int c = snd_sfx_chn_alloc();
		if (c < 0 || (first >= 0 && c != first + i)) { dbglog(DBG_ERROR, "BD: canaux AICA non contigus, voix materielles desactivees\n"); return false; }
		if (first < 0) first = c;
	}
	g_hwOps.chanBase = first;
	Snd::setHwOps(&g_hwOps);
	dbglog(DBG_INFO, "BD: voix materielles AICA : canaux %d a %d\n", first, first + Snd::MAX_VOICES - 1);
	return true;
}
#else
static void hwTick() {}
static bool hwInit() { return false; }
#endif

static void *speechCb(snd_stream_hnd_t, int bytesReq, int *bytesRecv) {
	int ch = Snd::Mixer::get().speechStereo() ? 2 : 1;
	int frames = bytesReq / (2 * ch);
	if (frames > 32768) frames = 32768;
	if (frames < 0) frames = 0;
	Snd::Mixer::get().renderSpeechNative(g_spOut, frames);
	*bytesRecv = frames * 2 * ch;
	return g_spOut;
}

static void speechService() {
	int ev; bool active; bool stereo;
	{ Snd::Lock lk; Snd::Mixer &m = Snd::Mixer::get(); ev = m.takeSpeechEvent(); active = m.speechActive(); stereo = m.speechStereo(); }
	if (ev && g_speechRunning) { snd_stream_stop(g_speech); g_speechRunning = false; }
	if (ev == 1 && active) {
		int real; { Snd::Lock lk; real = Snd::Mixer::get().speechRealFrames(); }
		snd_stream_start(g_speech, 37800, stereo ? 1 : 0); g_speechRunning = true;
		g_spT0 = timer_ms_gettime64(); g_spRealMs = (uint64_t)real * 1000 / 37800;
	}
	/* fin de la parole : l'AICA a joue toutes les donnees reelles (le tampon se vide en temps reel a partir du depart du flux) */
	if (g_speechRunning && active) {
		bool ended; { Snd::Lock lk; ended = Snd::Mixer::get().speechEnded(); }
		uint64_t now = timer_ms_gettime64();
		/* garde-fou : si la fin n'est jamais signalee (flux AICA bloque), on termine 1,5 s apres la duree reelle,
		   sinon le drapeau moteur 0x8000 reste pose et le personnage ne repond plus */
		if ((ended && now >= g_spT0 + g_spRealMs + 40) || now >= g_spT0 + g_spRealMs + 1500) {
			if (!ended) printf("BD: parole debloquee par le garde-fou (%u ms)\n", (unsigned)g_spRealMs);
			Snd::Lock lk; Snd::Mixer::get().finishSpeech(); active = false;
		}
	}
	/* parole active sans flux en cours (evenement de depart perdu) : on la termine au bout de 500 ms */
	static uint64_t s_orphan = 0;
	if (active && !g_speechRunning) {
		uint64_t now = timer_ms_gettime64();
		if (!s_orphan) s_orphan = now;
		else if (now >= s_orphan + 500) { printf("BD: parole orpheline terminee\n"); Snd::Lock lk; Snd::Mixer::get().finishSpeech(); active = false; s_orphan = 0; }
	} else s_orphan = 0;
	if (g_speechRunning && !active) { snd_stream_stop(g_speech); g_speechRunning = false; }
	if (g_speechRunning) snd_stream_poll(g_speech);
}

static void *pollThread(void *) {
	while (g_run) {
		snd_stream_poll(g_stream);
		speechService();
		hwTick();
		thd_sleep(8);
	}
	return nullptr;
}

bool dcAudioStart() {
	Snd::Hooks h = { lockFn, unlockFn };
	Snd::setHooks(h);
	if (snd_stream_init() < 0) { dbglog(DBG_ERROR, "BD: snd_stream_init a echoue\n"); return false; }
	/* 64 Ko par canal = 32768 trames (740 ms a 44,1 kHz ; refill par moitie = 16384 trames) : marge contre les pauses du thread principal (lecture disque d'une replique) */
	hwInit();   /* reserve les canaux AICA des voix avant les flux */
#ifdef BD_HWAUDIO
	/* Voix materielles : le flux principal ne porte plus que le son des videos STR. Son tampon est aussi un RETARD (l'AICA joue
	   ce que le melangeur a rendu 1 a 2 demi-tampons plus tot) : 64 Ko = 0,4 a 0,7 s de decalage voix/image. On le reduit. */
	const int kMainBuf = 8192;                     /* 4096 trames = 93 ms ; remplissage par moitie (46 ms) */
#else
	const int kMainBuf = 65536;                    /* musique mixee en logiciel : grand tampon contre les pauses du jeu */
#endif
	g_stream = snd_stream_alloc(streamCb, kMainBuf);
	if (g_stream == SND_STREAM_INVALID) { dbglog(DBG_ERROR, "BD: snd_stream_alloc a echoue\n"); return false; }
	g_speech = snd_stream_alloc(speechCb, kSpeechBuf);
	if (g_speech == SND_STREAM_INVALID) { dbglog(DBG_ERROR, "BD: flux parole impossible, melange logiciel\n"); }
	else { snd_stream_volume(g_speech, 255); Snd::Mixer::get().setSpeechExternal(true, 0x7fffffff); }
	snd_stream_volume(g_stream, 255);
	snd_stream_start(g_stream, Snd::OUT_RATE, 1);
	g_run = true;
	kthread_t *t = thd_create(true, pollThread, nullptr);
	if (!t) { dbglog(DBG_ERROR, "BD: thread audio impossible\n"); return false; }
	thd_set_prio(t, PRIO_DEFAULT - 2);   /* passe avant le jeu : le son ne s'interrompt pas quand une image est longue */
	dbglog(DBG_INFO, "BD: audio ok (%d Hz stereo, mixeur logiciel %d voix)\n", Snd::OUT_RATE, Snd::MAX_VOICES);
	return true;
}

/* Pour le journal : temps de mélange depuis le dernier appel, trames rendues, voix actives. */
#ifdef BD_HWAUDIO
void dcAudioHwStats(unsigned *uploads, unsigned *fails, unsigned *kb) { *uploads = g_hwUploads; *fails = g_hwFails; *kb = g_hwBytes / 1024; }
#else
void dcAudioHwStats(unsigned *uploads, unsigned *fails, unsigned *kb) { *uploads = *fails = *kb = 0; }
#endif

void dcAudioStats(unsigned long *msRender, unsigned long *frames, int *voices) {
	*msRender = (unsigned long)g_msRender; *frames = (unsigned long)g_framesRendered;
	g_msRender = 0; g_framesRendered = 0;
	*voices = Snd::Mixer::get().activeVoices();
}

/* plus long intervalle (ms) entre deux remplissages du flux depuis le dernier appel : proche de 370 ms = risque de coupure */
unsigned long dcAudioMaxGap() { unsigned long g = (unsigned long)g_maxGap; g_maxGap = 0; return g; }
