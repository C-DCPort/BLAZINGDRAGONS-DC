#include "snd.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>

namespace Snd {

static Hooks g_hooks = { nullptr, nullptr };
void setHooks(const Hooks &h) { g_hooks = h; }
Lock::Lock() { if (g_hooks.lock) g_hooks.lock(); }
Lock::~Lock() { if (g_hooks.unlock) g_hooks.unlock(); }

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ------------------------------------------------------------------ */
/* ADPCM PlayStation (VAG)                                             */
static const int kF0[5] = { 0, 60, 115, 98, 122 };
static const int kF1[5] = { 0, 0, -52, -55, -60 };

static void decodeVag(const u8 *b, s16 *out, s32 &s1, s32 &s2) {
	int shift = b[0] & 0xf; int filter = (b[0] >> 4) & 7;
	if (shift > 12) shift = 9;
	if (filter > 4) filter = 4;
	int f0 = kF0[filter], f1 = kF1[filter];
	for (int j = 0; j < 28; j++) {
		int nib = (b[2 + (j >> 1)] >> ((j & 1) ? 4 : 0)) & 0xf;
		int v = (int)(s16)(nib << 12) >> shift;
		int s = v + ((s1 * f0 + s2 * f1 + 32) >> 6);
		s = clampi(s, -32768, 32767);
		s2 = s1; s1 = s;
		out[j] = (s16)s;
	}
}

/* VAG -> PCM : on suit les blocs comme advanceBlock (drapeau 4 = debut de boucle, 1 = fin, 2 = boucle). Le decodage de la boucle
   reprend avec l'etat memorise au debut de boucle, donc il redonne exactement les memes echantillons : le PCM lineaire suffit. */
u32 vagToPcm(const u8 *vag, size_t size, s16 *out, u32 maxSamples, u32 *loopStart, bool *loop) {
	size_t nb = size / 16; u32 n = 0; s32 s1 = 0, s2 = 0; u32 ls = 0; *loop = false;
	for (size_t b = 0; b < nb; b++) {
		if (n + 28 > maxSamples) return 0;
		const u8 *p = vag + b * 16; int flags = p[1];
		if (flags & 4) ls = n;
		decodeVag(p, out + n, s1, s2); n += 28;
		if (flags & 1) { *loop = (flags & 2) != 0; break; }
	}
	*loopStart = ls;
	return n;
}

/* Pre-filtre passe-bas (equivalent approche du lissage de l interpolation gaussienne du SPU) applique aux echantillons envoyes a l'AICA :
   son interpolation lineaire laisse sinon trop d'aigus (son metallique). Les bords bouclent comme la boucle. */
void smoothPcm(s16 *p, u32 n, u32 loopStart, bool loop) {
	if (n < 4) return;
	std::vector<s16> t(p, p + n);
	auto at = [&](long i) -> int {
		if (i < 0) return t[0];
		if (i >= (long)n) return loop ? t[loopStart + (u32)(i - n) % (n - loopStart)] : t[n - 1];
		return t[i];
	};
	for (u32 i = 0; i < n; i++) {
		int v = (at((long)i - 1) * 4096 + at(i) * 24576 + at((long)i + 1) * 4096) >> 15;
		p[i] = (s16)clampi(v, -32768, 32767);
	}
}

/* ------------------------------------------------------------------ */
/* Banque VAB                                                          */
static inline u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static inline u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }

static const HwOps *g_hw = nullptr;
void setHwOps(const HwOps *ops) { g_hw = ops; }
VabBank::~VabBank() { if (g_hw && _body) g_hw->invalidate(_body, _bodySize); free(_body); delete[] _tones; }

bool VabBank::parseHeader(const u8 *vh, size_t n, size_t *headerEnd) {
	if (n < 32 + 2048 || memcmp(vh, "pBAV", 4) != 0) return false;
	_numProg = rd16(vh + 18); _numVag = rd16(vh + 22);
	_masterVol = vh[24];
	if (_numProg > 128) _numProg = 128;
	size_t need = 32 + 2048 + (size_t)_numProg * 16 * 32 + 512;
	if (n < need) return false;
	for (int i = 0; i < 128; i++) { _progTones[i] = vh[32 + i * 16]; _progVol[i] = vh[32 + i * 16 + 1]; }
	delete[] _tones;
	_tones = new Tone[_numProg * 16];
	const u8 *t = vh + 32 + 2048;
	for (int i = 0; i < _numProg * 16; i++, t += 32) {
		_tones[i].vol = t[2]; _tones[i].pan = t[3]; _tones[i].center = t[4]; _tones[i].shift = t[5];
		_tones[i].minKey = t[6]; _tones[i].maxKey = t[7];
		_tones[i].adsr1 = rd16(t + 16); _tones[i].adsr2 = rd16(t + 18);
		_tones[i].vag = (int)(int16_t)rd16(t + 22);
	}
	/* table des tailles : entrée 0 réservée, entrées 1.. = taille/8 des VAG 1.. */
	u32 off = rd16(t) << 3;
	_vagOff[0] = off;
	for (int j = 1; j < 256; j++) {
		u32 sz = rd16(t + j * 2) << 3;
		_vagSize[j - 1] = sz;
		_vagOff[j] = _vagOff[j - 1] + sz;
	}
	*headerEnd = need;
	return true;
}

bool VabBank::load(const u8 *vh, size_t vhSize, const u8 *vb, size_t vbSize) {
	size_t e;
	if (!parseHeader(vh, vhSize, &e)) return false;
	if (g_hw && _body) g_hw->invalidate(_body, _bodySize);
	free(_body);
	_body = (u8 *)malloc(vbSize ? vbSize : 1);
	if (!_body) return false;
	memcpy(_body, vb, vbSize); _bodySize = vbSize;
	return true;
}
bool VabBank::loadCombined(const u8 *d, size_t n) {
	size_t e;
	if (!parseHeader(d, n, &e)) return false;
	if (g_hw && _body) g_hw->invalidate(_body, _bodySize);
	free(_body);
	size_t bs = n - e;
	_body = (u8 *)malloc(bs ? bs : 1);
	if (!_body) return false;
	memcpy(_body, d + e, bs); _bodySize = bs;
	return true;
}
int VabBank::find(int program, int key, const Tone **out, int maxOut) const {
	if (program < 0 || program >= _numProg) return 0;
	int n = 0;
	int cnt = _progTones[program]; if (cnt > 16) cnt = 16;
	for (int t = 0; t < cnt && n < maxOut; t++) {
		const Tone &tn = _tones[program * 16 + t];
		if (tn.vag > 0 && tn.minKey <= key && key <= tn.maxKey) out[n++] = &tn;
	}
	return n;
}
const u8 *VabBank::vag(int index, size_t *size) const {
	if (index < 0 || index >= 255 || !_body) return nullptr;
	u32 o = _vagOff[index], s = _vagSize[index];
	if (o >= _bodySize) return nullptr;
	if (o + s > _bodySize) s = (u32)(_bodySize - o);
	*size = s;
	return _body + o;
}

/* ------------------------------------------------------------------ */
/* Enveloppe ADSR (modèle du SPU PlayStation, un tick à 44100 Hz)      */
static void envRate(bool exp, bool decrease, int shift, int step, s32 level, s32 &cycles, s32 &stepVal) {
	int base = decrease ? (-8 + step) : (7 - step);
	cycles = 1 << (shift > 11 ? shift - 11 : 0);
	stepVal = base << (shift < 11 ? 11 - shift : 0);
	if (exp) {
		if (decrease) stepVal = (stepVal * level) >> 15;
		else if (level > 0x6000) cycles <<= 2;
	}
}

static void envTick(Voice &v) {
	if (v.cycles > 0) { v.cycles--; return; }
	s32 cyc, st;
	switch (v.phase) {
	case 0:
		envRate((v.adsr1 >> 15) & 1, false, (v.adsr1 >> 10) & 31, (v.adsr1 >> 8) & 3, v.level, cyc, st);
		v.cycles = cyc - 1; v.level += st;
		if (v.level >= 0x7fff) { v.level = 0x7fff; v.phase = 1; v.cycles = 0; }
		break;
	case 1: {
		envRate(true, true, (v.adsr1 >> 4) & 15, 0, v.level, cyc, st);
		v.cycles = cyc - 1; v.level += st;
		s32 target = ((v.adsr1 & 15) + 1) * 0x800;
		if (v.level < 0) v.level = 0;
		if (v.level <= target) { v.phase = 2; v.cycles = 0; }
		break; }
	case 2:
		envRate((v.adsr2 >> 15) & 1, (v.adsr2 >> 14) & 1, (v.adsr2 >> 8) & 31, (v.adsr2 >> 6) & 3, v.level, cyc, st);
		v.cycles = cyc - 1; v.level += st;
		if (v.level > 0x7fff) v.level = 0x7fff;
		if (v.level <= 0) { v.level = 0; v.phase = 4; }
		break;
	case 3:
		envRate((v.adsr2 >> 5) & 1, true, v.adsr2 & 31, 0, v.level, cyc, st);
		v.cycles = cyc - 1; v.level += st;
		if (v.level <= 0) { v.level = 0; v.phase = 4; }
		break;
	}
}

/* ------------------------------------------------------------------ */
Mixer &Mixer::get() { static Mixer m; return m; }
int Mixer::speechBoost = 4096;

Mixer::Mixer() : _musicVol(255), _sfxVol(255), _speechVol(255), _duck(false), _seq(nullptr), _age(0), statSpeech(0), statSfx(0), statNotes(0), statClip(0), statStrUnder(0) {
	memset(_v, 0, sizeof(_v)); memset(_hwLive, 0, sizeof(_hwLive)); memset(&_xa, 0, sizeof(_xa)); memset(&_str, 0, sizeof(_str));
	for (int i = 0; i < 16; i++) _bend[i] = 0;
}
int Mixer::activeVoices() { int n = 0; for (auto &v : _v) n += v.active; return n; }

int Mixer::allocVoice(int group) {
	for (int i = 0; i < MAX_VOICES; i++) if (!_v[i].active) return i;
	int best = -1; s64 bestScore = -1;
	for (int i = 0; i < MAX_VOICES; i++) {
		Voice &v = _v[i];
		/* On vole d'abord les notes en relâchement, puis la plus ancienne (effets avant musique) */
		s64 score = (v.phase == 3 ? (s64)1 << 40 : 0) + (v.group == 1 ? (s64)1 << 30 : 0) + (s64)(0x7fffffff - (s64)(v.age & 0x7fffffff));
		if (v.phase == 3) score += (s64)(0x7fff - v.level);
		if (score > bestScore) { bestScore = score; best = i; }
	}
	return best;
}

static void applyGain(Voice &v) {
	s64 g = ((s64)v.baseGain * v.chanGain) >> 12;
	int pan = clampi(v.chanPan + v.tonePan - 64, 0, 127);
	int l = pan > 64 ? 128 - pan : 64, r = pan < 64 ? pan : 64;   /* 64 = plein */
	v.gainL = (s32)((g * l) >> 6); v.gainR = (s32)((g * r) >> 6);
}

int Mixer::startVag(const u8 *data, size_t size, int tag, int group, int key, int center,
                    int baseGainQ12, int tonePan, int chanGainQ12, int chanPan,
                    bool env, u16 adsr1, u16 adsr2, int chan) {
	if (!data || size < 16) return -1;
	int i = allocVoice(group);
	if (i < 0) return -1;
	Voice &v = _v[i];
	memset(&v, 0, sizeof(v));
	v.tag = tag; v.group = group; v.age = ++_age;
	float st = exp2f((float)(key - center) / 12.0f) * (44100.0f / OUT_RATE) * 65536.0f;
	v.baseStep = (u32)st;
	float bend = (chan >= 0 && chan < 16) ? _bend[chan] : 0.f;
	v.step = bend == 0.f ? v.baseStep : (u32)(st * exp2f(bend / 12.0f));
	v.baseGain = baseGainQ12; v.chanGain = chanGainQ12; v.tonePan = tonePan; v.chanPan = chanPan;
	applyGain(v);
	v.data = data; v.numBlocks = size / 16; v.blk = 0;
	v.loopBlk = 0; v.loopS1 = v.loopS2 = 0;
	v.chan = chan; v.key = key;
	v.useEnv = env; v.adsr1 = adsr1; v.adsr2 = adsr2;
	if (env) { v.phase = 0; v.level = 0; v.cycles = 0; } else { v.phase = 2; v.level = 0x7fff; }
	/* remplit les deux blocs d'avance */
	auto decodeNext = [&](s16 *dst, bool &isLast) {
		isLast = false;
		if (v.dataEnded || v.blk >= v.numBlocks) {
			if (!v.dataEnded) { v.dataEnded = true; isLast = true; }
			memset(dst, 0, 28 * sizeof(s16));
			return;
		}
		const u8 *b = v.data + v.blk * 16; int flags = b[1];
		if (flags & 4) { v.loopBlk = v.blk; v.loopS1 = v.s1; v.loopS2 = v.s2; }
		decodeVag(b, dst, v.s1, v.s2);
		if (flags & 1) {
			if (flags & 2) { v.blk = v.loopBlk; v.s1 = v.loopS1; v.s2 = v.loopS2; }
			else { v.dataEnded = true; isLast = true; }
		} else v.blk++;
	};
	v.buf[0] = v.buf[1] = v.buf[2] = 0;
	decodeNext(v.buf + 3, v.curLast);
	decodeNext(v.buf + 31, v.nextLast);
	v.active = true;
	if (g_hw) {
		v.hw = g_hw->upload(data, size, &v.hwS);
		if (v.hw) { v.hwPending = true; v.hwPos = 0; }
	}
	if (group == 1) statSfx++; else statNotes++;
	return i;
}

/* Interpolation gaussienne 4 points, comme la puce son de la PlayStation : nettement plus douce dans les aigus que
   l'interpolation lineaire (qui laissait un son metallique). 256 phases, poids Q15 de somme 32768. */
static s16 g_gauss[256][4];
static bool g_gaussReady = false;
static void initGauss() {
	const float sigma = 0.62f;
	for (int p = 0; p < 256; p++) {
		float t = p / 256.0f, w[4], sum = 0;
		for (int k = 0; k < 4; k++) { float d = (k - 1) - t; w[k] = expf(-d * d / (2 * sigma * sigma)); sum += w[k]; }
		int acc = 0;
		for (int k = 0; k < 4; k++) { g_gauss[p][k] = (s16)(w[k] / sum * 32768.0f + 0.5f); acc += g_gauss[p][k]; }
		g_gauss[p][1] += (s16)(32768 - acc);
	}
	g_gaussReady = true;
}
static inline s32 interp4(s32 a, s32 b, s32 c, s32 d, u32 frac16) {
	const s16 *w = g_gauss[frac16 >> 8];
	return (a * w[0] + b * w[1] + c * w[2] + d * w[3]) >> 15;
}

static void advanceBlock(Voice &v) {
	if (v.curLast) { v.active = false; return; }
	memmove(v.buf, v.buf + 28, 31 * sizeof(s16));   /* garde les 3 derniers echantillons comme historique */
	v.curLast = v.nextLast;
	/* décode le bloc suivant */
	s16 *dst = v.buf + 31; v.nextLast = false;
	if (v.dataEnded || v.blk >= v.numBlocks) {
		if (!v.dataEnded) { v.dataEnded = true; v.nextLast = true; }
		memset(dst, 0, 28 * sizeof(s16));
		return;
	}
	const u8 *b = v.data + v.blk * 16; int flags = b[1];
	if (flags & 4) { v.loopBlk = v.blk; v.loopS1 = v.s1; v.loopS2 = v.s2; }
	decodeVag(b, dst, v.s1, v.s2);
	if (flags & 1) {
		if (flags & 2) { v.blk = v.loopBlk; v.s1 = v.loopS1; v.s2 = v.loopS2; }
		else { v.dataEnded = true; v.nextLast = true; }
	} else v.blk++;
}

void Mixer::mixVoice(Voice &v, s32 *acc, int frames, s32 groupGain) {
	s32 gl = (v.gainL * groupGain) >> 12, gr = (v.gainR * groupGain) >> 12;
	const u32 tickInc = (u32)((44100.0 / OUT_RATE) * 65536.0);
	const u32 blockEnd = 28u << 16;
	int i = 0;
	while (i < frames && v.active) {
		/* nombre d'echantillons jusqu'au prochain changement de bloc : la boucle interne n'a plus a tester la fin de bloc */
		u32 room = blockEnd - v.pos;
		int n = (int)((room + v.step - 1) / v.step);
		if (n < 1) n = 1;
		if (n > frames - i) n = frames - i;
		const s16 *base = v.buf + 2;
		s32 *a = acc + 2 * i;
		u32 pos = v.pos;
		if (v.useEnv) {
			for (int k = 0; k < n; k++) {
				v.tickAcc += tickInc;
				while (v.tickAcc >= 65536) { v.tickAcc -= 65536; envTick(v); }
				if (v.phase == 4) { v.active = false; v.pos = pos; goto done; }
				const s16 *q = base + (pos >> 16);
				s32 s = interp4(q[0], q[1], q[2], q[3], pos & 0xffff);
				s = (s * v.level) >> 15;
				a[2 * k] += (s * gl) >> 12; a[2 * k + 1] += (s * gr) >> 12;
				pos += v.step;
			}
		} else {
			for (int k = 0; k < n; k++) {
				const s16 *q = base + (pos >> 16);
				s32 s = interp4(q[0], q[1], q[2], q[3], pos & 0xffff);
				a[2 * k] += (s * gl) >> 12; a[2 * k + 1] += (s * gr) >> 12;
				pos += v.step;
			}
		}
		v.pos = pos; i += n;
		while (v.pos >= blockEnd) { v.pos -= blockEnd; advanceBlock(v); if (!v.active) break; }
	}
done:;
}

/* ---- XA (parole) : algorithme identique à l'original ---- */
static const int kXa[5][2] = { { 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }, { 122, -60 } };
/* Decalage d'un bloc XA : parametre 0..12 ; 13..15 sont reserves et se comportent comme 9 sur PlayStation.
   (12 - 13 = -1 : un decalage negatif est indefini en C ; le SH4 decale alors a droite, x86 non -> parasites sur Dreamcast.) */
static inline int xaShift(u8 hdr) { int p = hdr & 0xf; if (p > 12) p = 9; return 12 - p; }

static void decodeXaSector(const u8 *buf, s16 *dst, bool stereo, s32 st[2][2]) {
	int channels = stereo ? 2 : 1;
	s16 *left = dst, *right = dst + 1;
	for (const u8 *src = buf; src < buf + 2304; src += 128) {
		for (int i = 0; i < 4; i++) {
			int shift = xaShift(src[4 + i * 2]);
			int filter = (src[4 + i * 2] >> 4) & 7; if (filter > 4) filter = 4;
			int f0 = kXa[filter][0], f1 = kXa[filter][1];
			int s_1 = st[0][0], s_2 = st[0][1];
			for (int j = 0; j < 28; j++) {
				u8 d = src[16 + i + j * 4];
				int t = (int8_t)(d << 4) >> 4;
				int s = (t << shift) + ((s_1 * f0 + s_2 * f1 + 32) >> 6);
				s_2 = s_1; s_1 = clampi(s, -32768, 32767);
				*left = (s16)s_1; left += channels;
			}
			if (stereo) { st[0][0] = s_1; st[0][1] = s_2; s_1 = st[1][0]; s_2 = st[1][1]; }
			shift = xaShift(src[5 + i * 2]);
			filter = (src[5 + i * 2] >> 4) & 7; if (filter > 4) filter = 4;
			f0 = kXa[filter][0]; f1 = kXa[filter][1];
			for (int j = 0; j < 28; j++) {
				u8 d = src[16 + i + j * 4];
				int t = (int8_t)d >> 4;
				int s = (t << shift) + ((s_1 * f0 + s_2 * f1 + 32) >> 6);
				s_2 = s_1; s_1 = clampi(s, -32768, 32767);
				if (stereo) { *right = (s16)s_1; right += 2; } else { *left++ = (s16)s_1; }
			}
			if (stereo) { st[1][0] = s_1; st[1][1] = s_2; } else { st[0][0] = s_1; st[0][1] = s_2; }
		}
	}
}

void Mixer::startSpeech(const u8 *data, int sectors, bool stereo, bool rate18900, int gainQ12) {
	XaVoice &x = _xa;
	memset(&x, 0, sizeof(x));
	if (!data || sectors <= 0) return;
	x.data = data; x.sectors = sectors; x.stereo = stereo;
	x.framesPerSec = stereo ? 2016 : 4032;
	int rate = rate18900 ? 18900 : 37800;
	x.step = (u32)(((double)rate / OUT_RATE) * 65536.0);
	x.gainL = x.gainR = gainQ12;
	decodeXaSector(data, x.cur, stereo, x.st);
	if (sectors > 1) decodeXaSector(data + 2304, x.next, stereo, x.st); else memset(x.next, 0, sizeof(x.next));
	x.up2 = rate18900; x.ended = false; x.padFrames = 0;
	x.sec = 0; x.pos = 0; x.active = true; statSpeech++;
	if (_xaExternal) { x.step = 0; _spEvent = 1; }
}

void Mixer::mixXa(s32 *acc, int frames) {
	XaVoice &x = _xa;
	s32 groupGain = (((_speechVol * 4096) / 255) * speechBoost) >> 12;
	s32 gl = (x.gainL * groupGain) >> 12, gr = (x.gainR * groupGain) >> 12;
	int ch = x.stereo ? 2 : 1;
	for (int i = 0; i < frames && x.active; i++) {
		int idx = x.pos >> 16; s32 frac = x.pos & 0xffff;
		/* echantillon n du canal c, n de -1 (secteur precedent) a framesPerSec + 1 (secteur suivant) */
		auto smp = [&](int n, int c) -> s32 {
			if (n < 0) return x.hist[c][3 + n];
			if (n >= x.framesPerSec) return x.next[(n - x.framesPerSec) * ch + c];
			return x.cur[n * ch + c];
		};
		s32 sl, sr;
		if (idx >= 1 && idx + 2 < x.framesPerSec) {   /* chemin rapide : les 4 points sont dans le secteur courant */
			const s16 *q = x.cur + (idx - 1) * ch;
			const s16 *w = g_gauss[frac >> 8];
			sl = (q[0] * w[0] + q[ch] * w[1] + q[2 * ch] * w[2] + q[3 * ch] * w[3]) >> 15; sr = sl;
			if (x.stereo) sr = (q[1] * w[0] + q[ch + 1] * w[1] + q[2 * ch + 1] * w[2] + q[3 * ch + 1] * w[3]) >> 15;
		} else {
		sl = interp4(smp(idx - 1, 0), smp(idx, 0), smp(idx + 1, 0), smp(idx + 2, 0), frac); sr = sl;
		if (x.stereo) sr = interp4(smp(idx - 1, 1), smp(idx, 1), smp(idx + 1, 1), smp(idx + 2, 1), frac);
		}
		acc[2 * i] += (sl * gl) >> 12; acc[2 * i + 1] += (sr * gr) >> 12;
		x.pos += x.step;
		while (x.pos >= ((u32)x.framesPerSec << 16)) {
			x.pos -= (u32)x.framesPerSec << 16;
			for (int c = 0; c < ch; c++) for (int k = 0; k < 3; k++) x.hist[c][k] = x.cur[(x.framesPerSec - 3 + k) * ch + c];
			memcpy(x.cur, x.next, sizeof(x.cur));
			x.sec++;
			if (x.sec >= x.sectors) { x.active = false; break; }
			if (x.sec + 1 < x.sectors) decodeXaSector(x.data + (size_t)(x.sec + 1) * 2304, x.next, x.stereo, x.st);
			else memset(x.next, 0, sizeof(x.next));
		}
	}
}

/* Parole pour le flux AICA separe : sortie a 37,8 kHz. La source 18,9 kHz est sur-echantillonnee x2 par l'interpolation
   gaussienne (phases 0 et 1/2), la source 37,8 kHz est copiee telle quelle. Apres la fin des donnees on envoie du silence
   pendant la latence du tampon AICA, puis la parole est declaree terminee (le jeu attend la fin reelle du son). */
int Mixer::renderSpeechNative(s16 *out, int frames) {
	if (!g_gaussReady) initGauss();
	Lock lk;
	XaVoice &x = _xa;
	int ch = x.stereo ? 2 : 1;
	/* -2,5 dB : sur Dreamcast le melange parole + musique se fait dans l'AICA, sans limiteur ; une parole a pleine echelle
	   (cretes a 31000) ecreterait des que la musique s'y ajoute (claquements). */
	s32 g = (((((_speechVol * 4096) / 255 * x.gainL) >> 12) * 3 / 4) * speechBoost) >> 12;
	const s16 *w0 = g_gauss[0], *w1 = g_gauss[128];
	for (int i = 0; i < frames; i++) {
		if (!x.active || x.ended) {
			/* fin : la sortie decroit au lieu de tomber a zero d'un coup (claquement) */
			for (int c = 0; c < ch; c++) { x.lastOut[c] = (s16)(x.lastOut[c] * 7 / 8); out[i * ch + c] = x.lastOut[c]; }
			if (x.active && _spLatency > 0 && ++x.padFrames >= _spLatency) x.active = false;
			continue;
		}
		int idx = (int)x.pos;   /* ici pos = indice entier dans le secteur courant, step = phase (0 ou 1) */
		auto smp = [&](int n, int c) -> s32 {
			if (n < 0) return x.hist[c][3 + n];
			if (n >= x.framesPerSec) return x.next[(n - x.framesPerSec) * ch + c];
			return x.cur[n * ch + c];
		};
		for (int c = 0; c < ch; c++) {
			s32 v;
			if (x.up2) {
				const s16 *w = x.step ? w1 : w0;
				v = (smp(idx - 1, c) * w[0] + smp(idx, c) * w[1] + smp(idx + 1, c) * w[2] + smp(idx + 2, c) * w[3]) >> 15;
			} else v = smp(idx, c);
			v = (v * g) >> 12;
			if (v > 24576) v = 24576 + (v - 24576) / 4; else if (v < -24576) v = -24576 + (v + 24576) / 4;   /* crêtes adoucies (gain anglais) */
			if (x.fadeIn < 64) v = v * x.fadeIn / 64;
			out[i * ch + c] = (s16)clampi(v, -32768, 32767);
			x.lastOut[c] = out[i * ch + c];
		}
		if (x.fadeIn < 64) x.fadeIn++;
		if (x.up2 && x.step == 0) { x.step = 1; continue; }
		x.step = 0; x.pos++;
		if ((int)x.pos >= x.framesPerSec) {
			x.pos = 0;
			for (int c = 0; c < ch; c++) for (int k = 0; k < 3; k++) x.hist[c][k] = x.cur[(x.framesPerSec - 3 + k) * ch + c];
			memcpy(x.cur, x.next, sizeof(x.cur));
			x.sec++;
			if (x.sec >= x.sectors) { x.ended = true; continue; }
			if (x.sec + 1 < x.sectors) decodeXaSector(x.data + (size_t)(x.sec + 1) * 2304, x.next, x.stereo, x.st);
			else memset(x.next, 0, sizeof(x.next));
		}
	}
	return frames;
}

/* ---- flux PCM (vidéos) ---- */
bool Mixer::streamBegin() {
	Lock lk;
	if (!_str.ring) { _str.ring = (s16 *)malloc((size_t)STREAM_FRAMES * 2 * sizeof(s16)); if (!_str.ring) return false; }
	s16 *r = _str.ring;
	memset(&_str, 0, sizeof(_str)); _str.ring = r;
	_str.gain = 4096; _str.step = 65536; _str.active = true;
	return true;
}
void Mixer::Lock2() { Lock lk; _str.ended = true; }
void Mixer::streamStop() { Lock lk; _str.active = false; }
int Mixer::streamQueued() { Lock lk; return (int)(_str.wr - _str.rd); }

void Mixer::streamPushXa(const u8 *xa, bool stereo, bool rate18900) {
	static s16 tmp[4032];
	Lock lk;
	StreamVoice &s = _str;
	if (!s.active || !s.ring) return;
	decodeXaSector(xa, tmp, stereo, s.st);
	int frames = stereo ? 2016 : 4032;
	s.step = (u32)(((double)(rate18900 ? 18900 : 37800) / OUT_RATE) * 65536.0);
	if ((int)(s.wr - s.rd) + frames > STREAM_FRAMES) return;          /* tampon plein : le secteur est perdu */
	for (int i = 0; i < frames; i++) {
		s16 l = stereo ? tmp[2 * i] : tmp[i], r = stereo ? tmp[2 * i + 1] : tmp[i];
		u32 w = (s.wr + i) & (STREAM_FRAMES - 1);
		s.ring[2 * w] = l; s.ring[2 * w + 1] = r;
	}
	s.wr += frames;
}

void Mixer::mixStream(s32 *acc, int frames) {
	StreamVoice &s = _str;
	/* Demarrage : on attend ~0,3 s d'avance. Apres une panne (decodage en retard) on attend de nouveau 4500 trames :
	   mieux vaut un silence net qu'une suite de micro-coupures, et la sortie decroit/remonte en douceur (pas de claquement). */
	if (!s.started) { if ((int)(s.wr - s.rd) < 9000 && !s.ended) return; s.started = true; s.fade = 0; }
	s32 g = ((_speechVol * 4096) / 255 * s.gain) >> 12;
	for (int i = 0; i < frames; i++) {
		int queued = (int)(s.wr - s.rd);
		if (queued < 2) { if (s.ended) { s.active = false; return; } if (!s.starving) { s.starving = true; statStrUnder++; } }
		if (s.starving) {
			if (queued >= 4500 || s.ended) { s.starving = false; s.fade = 0; }
			else {
				s.lastL = s.lastL * 7 / 8; s.lastR = s.lastR * 7 / 8;
				acc[2 * i] += (s.lastL * g) >> 12; acc[2 * i + 1] += (s.lastR * g) >> 12;
				continue;
			}
		}
		u32 a = s.rd & (STREAM_FRAMES - 1), b = (s.rd + 1) & (STREAM_FRAMES - 1);
		s32 f = s.pos & 0xffff;
		s32 l = s.ring[2 * a] + (((s.ring[2 * b] - s.ring[2 * a]) * f) >> 16);
		s32 r = s.ring[2 * a + 1] + (((s.ring[2 * b + 1] - s.ring[2 * a + 1]) * f) >> 16);
		if (s.fade < 64) { l = (l * s.fade) >> 6; r = (r * s.fade) >> 6; s.fade++; }
		s.lastL = l; s.lastR = r;
		acc[2 * i] += (l * g) >> 12; acc[2 * i + 1] += (r * g) >> 12;
		s.pos += s.step;
		u32 adv = s.pos >> 16; s.pos &= 0xffff; s.rd += adv;
	}
}

/* ---- commandes ---- */
void Mixer::stopTag(int tag, int group) { for (auto &v : _v) if (v.active && v.group == group && v.tag == tag) v.active = false; }
bool Mixer::isTagActive(int tag, int group) { for (auto &v : _v) if (v.active && v.group == group && v.tag == tag) return true; return false; }
void Mixer::stopGroup(int group) { for (auto &v : _v) if (v.group == group) v.active = false; }
void Mixer::noteOff(int chan, int key, bool pedal) {
	for (auto &v : _v) {
		if (!v.active || v.group != 0 || v.chan != chan || v.key != key || v.phase >= 3) continue;
		if (pedal) { v.pendingOff = true; continue; }
		v.phase = 3; v.cycles = 0;
	}
}
void Mixer::pedalRelease(int chan) {
	for (auto &v : _v) if (v.active && v.group == 0 && v.chan == chan && v.pendingOff && v.phase < 3) { v.phase = 3; v.cycles = 0; v.pendingOff = false; }
}
void Mixer::releaseChan(int chan) {
	for (auto &v : _v) if (v.active && v.group == 0 && (chan < 0 || v.chan == chan) && v.phase < 3) { v.phase = 3; v.cycles = 0; }
}
void Mixer::updateChan(int chan, int chanGainQ12, int chanPan) {
	for (auto &v : _v) if (v.active && v.group == 0 && v.chan == chan) { v.chanGain = chanGainQ12; v.chanPan = chanPan; applyGain(v); }
}
void Mixer::setChanBend(int chan, float semis) {
	if (chan < 0 || chan > 15) return;
	_bend[chan] = semis;
	float f = exp2f(semis / 12.0f);
	for (auto &v : _v) if (v.active && v.group == 0 && v.chan == chan) v.step = (u32)((float)v.baseStep * f);
}

/* Avance l'enveloppe de `ticks` ticks SPU (44100 Hz) en sautant les attentes : le cout depend du nombre de marches, pas de la duree. */
static void envAdvance(Voice &v, u32 ticks) {
	while (ticks > 0 && v.phase != 4) {
		if (v.cycles > 0) { u32 t = (u32)v.cycles < ticks ? (u32)v.cycles : ticks; v.cycles -= (s32)t; ticks -= t; }
		else { envTick(v); ticks--; }
	}
}

/* gains de la voix -> frequence AICA, volume 0..255 et panoramique 0..255 (128 = centre) */
void Mixer::hwCompute(Voice &v, s32 groupGain, int *freq, int *vol, int *pan) {
	s32 gl = (v.gainL * groupGain) >> 12, gr = (v.gainR * groupGain) >> 12;
	s32 level = v.useEnv ? v.level : 0x7fff;
	s32 mx = gl > gr ? gl : gr, mn = gl > gr ? gr : gl;
	s64 a = ((s64)level * mx) >> 15;                 /* amplitude Q12 de la voie dominante */
	int vv = (int)((a * 255 + 2048) >> 12); if (vv > 255) vv = 255; if (vv < 0) vv = 0;
	*vol = vv;
	int p = 128;
	if (mx > 0 && mn < mx) {
		float r = (float)mn / (float)mx;
		int n = r <= 0.f ? 15 : (int)(-2.0f * log2f(r) + 0.5f); if (n > 15) n = 15;   /* pas de 3 dB */
		if (n > 0) p = gl > gr ? 127 - 8 * n : 128 + 8 * n;
	}
	*pan = p;
	*freq = (int)(((u64)v.step * OUT_RATE) >> 16); if (*freq < 100) *freq = 100;
}

void Mixer::hwService(int frames) {
	if (!g_hw || frames <= 0) return;
	Lock lk;
	if (_seq) { int left = frames; while (left > 0) { int n = left > 256 ? 256 : left; _seq->advance(n); left -= n; } }
	s32 musicGain = ((_musicVol * 4096) / 255) >> 1;
	if (_duck) musicGain = musicGain * 3 / 8;
	s32 sfxGain = ((_sfxVol * 4096) / 255) >> 1;
	for (int i = 0; i < MAX_VOICES; i++) {
		Voice &v = _v[i];
		int chan = g_hw->chanBase + i;
		if (!(v.active && v.hw)) { if (_hwLive[i]) { g_hw->stop(chan); _hwLive[i] = false; } continue; }
		s32 gg = v.group == 0 ? musicGain : sfxGain;
		if (v.hwPending) {
			int f, vo, pa; hwCompute(v, gg, &f, &vo, &pa);
			g_hw->play(chan, v.hwS, f, vo, pa);
			v.hwPending = false; _hwLive[i] = true; v.hwFreq = f; v.hwVol = vo; v.hwPan = pa;
			continue;
		}
		if (v.useEnv) envAdvance(v, (u32)frames);
		v.hwPos += (u64)v.step * (u32)frames;
		if (!v.hwS.loop && (v.hwPos >> 16) >= v.hwS.samples) { v.active = false; _hwLive[i] = false; continue; } /* fin naturelle : l'AICA s'arrete seule, pas de STOP (clic) */
		if (v.phase == 4) v.active = false;
		if (!v.active) { g_hw->stop(chan); _hwLive[i] = false; continue; }
		int f, vo, pa; hwCompute(v, gg, &f, &vo, &pa);
		int uf = f != v.hwFreq ? f : -1, uv = vo != v.hwVol ? vo : -1, up = pa != v.hwPan ? pa : -1;
		if (uf >= 0 || uv >= 0 || up >= 0) { g_hw->update(chan, uf, uv, up); v.hwFreq = f; v.hwVol = vo; v.hwPan = pa; }
	}
}

void Mixer::render(s16 *out, int frames) {
	if (!g_gaussReady) initGauss();
	int done = 0;
	while (done < frames) {
		int n = frames - done; if (n > 128) n = 128;
		s32 acc[128 * 2];
		memset(acc, 0, sizeof(s32) * 2 * n);
		{
			Lock lk;
			if (_seq && !g_hw) _seq->advance(n);
			/* Musique et effets (voix SPU) a -6 dB : a plein niveau la musique saturait (cretes a 32767) et couvrait
			   les dialogues (XA, gardes a leur niveau d'origine). Pendant un dialogue la musique baisse encore de 9 dB. */
			s32 musicGain = ((_musicVol * 4096) / 255) >> 1;
			if (_duck) musicGain = musicGain * 3 / 8;
			s32 sfxGain = ((_sfxVol * 4096) / 255) >> 1;
			for (auto &v : _v) if (v.active && !v.hw) mixVoice(v, acc, n, v.group == 0 ? musicGain : sfxGain);
			if (_xa.active && !_xaExternal) mixXa(acc, n);
			if (_str.active) mixStream(acc, n);
		}
		/* limiteur doux au-dela de 24576 : evite l'ecretage franc (son « metallique ») quand voix + musique s'additionnent */
		for (int i = 0; i < n * 2; i++) {
			int s = acc[i], a = s < 0 ? -s : s;
			if (a > 24576) {
				if (a > 32767) statClip++;
				const int R = 32767 - 24576, d = a - 24576;
				a = 24576 + (int)((s64)d * R / (d + R));
				s = s < 0 ? -a : a;
			}
			out[done * 2 + i] = (s16)s;
		}
		done += n;
	}
}

/* ------------------------------------------------------------------ */
/* Séquenceur                                                          */
Sequencer::~Sequencer() { free(_data); }

void Sequencer::stop() {
	if (_playing) { Mixer::get().releaseChan(-1); }
	_playing = false;
}

bool Sequencer::start(const u8 *seq, size_t size) {
	stop();
	if (size < 16 || memcmp(seq, "pQES", 4) != 0) return false;
	free(_data);
	_data = (u8 *)malloc(size); if (!_data) return false;
	memcpy(_data, seq, size); _size = size;
	_ppqn = (_data[8] << 8) | _data[9];
	_tempo = (_data[10] << 16) | (_data[11] << 8) | _data[12];
	if (_ppqn <= 0) _ppqn = 48;
	if (_tempo == 0) _tempo = 500000;
	_framesPerTick16 = (s64)_tempo * OUT_RATE * 65536LL / (1000000LL * _ppqn);
	for (int i = 0; i < 16; i++) { _prog[i] = 0; _vol[i] = 127; _pan[i] = 64; _expr[i] = 127; _sustain[i] = 0; }
	memset(_heldKeys, 0, sizeof(_heldKeys));
	_hasLoopPos = false; _loopPos = 15; _status = 0;
	_pos = 15;
	_acc16 = (s64)readVar() * _framesPerTick16;
	_playing = true;
	return true;
}

u32 Sequencer::readVar() {
	u32 v = 0;
	for (int i = 0; i < 4 && _pos < _size; i++) {
		u8 b = _data[_pos++];
		v = (v << 7) | (b & 0x7f);
		if (!(b & 0x80)) break;
	}
	return v;
}

void Sequencer::restart() {
	_pos = _hasLoopPos ? _loopPos : 15;
	_status = 0;
	_acc16 = (s64)readVar() * _framesPerTick16;
}

static int chanGainQ12(int vol, int expr) { return (int)((s64)4096 * vol * expr / (127 * 127)); }

void Sequencer::noteOn(int ch, int key, int vel) {
	const Tone *tones[8];
	int n = _bank->find(_prog[ch], key, tones, 8);
	Mixer &m = Mixer::get();
	for (int i = 0; i < n; i++) {
		const Tone *t = tones[i];
		size_t sz; const u8 *d = _bank->vag(t->vag - 1, &sz);
		if (!d) continue;
		/* gain de base : vélocité x ton x programme x général */
		s64 g = 4096;
		g = g * vel / 127; g = g * t->vol / 127;
		g = g * _bank->programVol(_prog[ch]) / 127; g = g * _bank->masterVol() / 127;
		m.startVag(d, sz, ch, 0, key, t->center, (int)g, t->pan, chanGainQ12(_vol[ch], _expr[ch]), _pan[ch],
		           true, t->adsr1, t->adsr2, ch);
	}
}
void Sequencer::noteOff(int ch, int key) { Mixer::get().noteOff(ch, key, _sustain[ch] >= 64); }

void Sequencer::event() {
	Mixer &m = Mixer::get();
	if (_pos >= _size) { restart(); return; }
	u8 b = _data[_pos];
	if (b >= 0x80) { _status = b; _pos++; }
	if (_status < 0x80) { restart(); return; }
	int hi = _status & 0xf0, ch = _status & 0x0f;
	if (hi == 0xf0) {
		if (_status == 0xff) {
			if (_pos + 1 > _size) { restart(); return; }
			u8 type = _data[_pos++];
			u32 len = readVar();
			if (type == 0x2f) { restart(); return; }
			if (type == 0x51 && len >= 3 && _pos + 3 <= _size) {
				_tempo = (_data[_pos] << 16) | (_data[_pos + 1] << 8) | _data[_pos + 2];
				if (_tempo) _framesPerTick16 = (s64)_tempo * OUT_RATE * 65536LL / (1000000LL * _ppqn);
			}
			_pos += len;
		} else if (_status == 0xf0 || _status == 0xf7) {
			u32 len = readVar(); _pos += len;
		}
		_status = _status == 0xff ? 0 : _status;
	} else {
		int d1 = _pos < _size ? _data[_pos++] : 0;
		int d2 = 0;
		if (hi != 0xc0 && hi != 0xd0) d2 = _pos < _size ? _data[_pos++] : 0;
		switch (hi) {
		case 0x80: noteOff(ch, d1); break;
		case 0x90: if (d2) noteOn(ch, d1, d2); else noteOff(ch, d1); break;
		case 0xb0:
			switch (d1) {
			case 7: _vol[ch] = d2; m.updateChan(ch, chanGainQ12(_vol[ch], _expr[ch]), _pan[ch]); break;
			case 11: _expr[ch] = d2; m.updateChan(ch, chanGainQ12(_vol[ch], _expr[ch]), _pan[ch]); break;
			case 10: _pan[ch] = d2; m.updateChan(ch, chanGainQ12(_vol[ch], _expr[ch]), _pan[ch]); break;
			case 64: _sustain[ch] = d2; if (d2 < 64) m.pedalRelease(ch); break;
			case 99:   /* marqueurs de boucle PlayStation */
				if (d2 == 20) { _hasLoopPos = true; _loopPos = _pos; }
				else if (d2 == 30 && _hasLoopPos) { _pos = _loopPos; _acc16 = (s64)readVar() * _framesPerTick16; return; }
				break;
			case 121: _vol[ch] = 127; _expr[ch] = 127; _pan[ch] = 64; _sustain[ch] = 0; m.setChanBend(ch, 0.f); break;
			case 123: case 120: m.releaseChan(ch); break;
			}
			break;
		case 0xc0: _prog[ch] = d1; break;
		case 0xe0: {
			int bend = ((d2 << 7) | d1) - 8192;
			m.setChanBend(ch, (float)bend * 2.0f / 8192.0f);
			break; }
		}
	}
	_acc16 = (s64)readVar() * _framesPerTick16;
}

void Sequencer::advance(int frames) {
	if (!_playing) return;
	s64 remain = (s64)frames << 16;
	int guard = 0;
	while (_playing) {
		if (_acc16 > remain) { _acc16 -= remain; break; }
		remain -= _acc16; _acc16 = 0;
		event();
		if (++guard > 5000) break;   /* sécurité contre une boucle sans durée */
	}
}

}  // namespace Snd
