#include "disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <string>

#ifdef __DREAMCAST__
#include <kos/fs.h>
#include <fcntl.h>
#include <malloc.h>
#endif

namespace Disc {

unsigned long long statCommands = 0, statBytes = 0;
static std::string s_dir = ".";
static void (*s_sleep)(int) = nullptr;
static volatile int s_foreground = 0;     /* lectures de premier plan en cours */

static const uint32_t SECTOR = 2048;
static const uint32_t FG_CHUNK = 64 * SECTOR;   /* 128 Ko par commande au premier plan */
static const uint32_t BG_CHUNK = 16 * SECTOR;   /*  32 Ko en arrière-plan : le premier plan n'attend jamais longtemps */

struct File {
#ifdef __DREAMCAST__
	file_t fd;
#else
	FILE *fp;
#endif
	uint8_t *buf;        /* tampon aligné (DC) */
	uint32_t chunk;
	long sz;
	bool bg;
};

void setDataDir(const char *dir) { s_dir = dir ? dir : "."; }
static std::string s_fallback;
void setDataFallback(const char *dir) { s_fallback = dir ? dir : ""; }
void setSleepHook(void (*sleepMs)(int)) { s_sleep = sleepMs; }

static File *open1(const std::string &dir, const char *name, bool background);
File *open(const char *name, bool background) {
	File *f = open1(s_dir, name, background);
	if (!f && !s_fallback.empty()) f = open1(s_fallback, name, background);
	return f;
}
static File *open1(const std::string &dir, const char *name, bool background) {
	std::string base = dir + "/";
	std::string variants[3] = { name, name, name };
	for (auto &c : variants[1]) c = toupper((unsigned char)c);
	for (auto &c : variants[2]) c = tolower((unsigned char)c);
	for (int i = 0; i < 3; i++) {
		std::string p = base + variants[i];
#ifdef __DREAMCAST__
		file_t fd = fs_open(p.c_str(), O_RDONLY);
		if (fd < 0) continue;
		File *f = new File();
		f->fd = fd;
		f->chunk = background ? BG_CHUNK : FG_CHUNK;
		f->buf = (uint8_t *)memalign(32, f->chunk + SECTOR);
		f->sz = (long)fs_total(fd);
		f->bg = background;
		if (!f->buf) { fs_close(fd); delete f; return nullptr; }
		return f;
#else
		FILE *fp = fopen(p.c_str(), "rb");
		if (!fp) continue;
		File *f = new File();
		f->fp = fp; f->buf = nullptr; f->chunk = background ? BG_CHUNK : FG_CHUNK; f->bg = background;
		fseek(fp, 0, SEEK_END); f->sz = ftell(fp);
		return f;
#endif
	}
	return nullptr;
}

void close(File *f) {
	if (!f) return;
#ifdef __DREAMCAST__
	fs_close(f->fd);
	free(f->buf);
#else
	fclose(f->fp);
#endif
	delete f;
}

long size(File *f) { return f ? f->sz : 0; }

long readAt(File *f, uint32_t off, void *dst, uint32_t n) {
	if (!f || n == 0) return 0;
	if (f->bg) { while (s_foreground > 0 && s_sleep) s_sleep(4); }
	else s_foreground++;
	long total = 0;
#ifdef __DREAMCAST__
	uint8_t *out = (uint8_t *)dst;
	while ((uint32_t)total < n) {
		uint32_t pos = off + total;
		uint32_t a = pos & ~(SECTOR - 1), skip = pos - a;
		uint32_t want = skip + (n - total);
		if (want > f->chunk) want = f->chunk;
		uint32_t rd = (want + SECTOR - 1) & ~(SECTOR - 1);
		if (fs_seek(f->fd, a, SEEK_SET) < 0) break;
		ssize_t got = fs_read(f->fd, f->buf, rd);
		if (got <= (ssize_t)skip) break;
		statCommands++; statBytes += (unsigned long long)got;
		uint32_t take = (uint32_t)got - skip;
		if (take > n - total) take = n - total;
		memcpy(out + total, f->buf + skip, take);
		total += take;
		if (got < (ssize_t)rd) break;          /* fin de fichier */
		if (f->bg && s_foreground > 0 && s_sleep) { while (s_foreground > 0) s_sleep(4); }
	}
#else
	fseek(f->fp, off, SEEK_SET);
	total = (long)fread(dst, 1, n, f->fp);
	statCommands++; statBytes += (unsigned long long)total;
#endif
	if (!f->bg) s_foreground--;
	return total;
}

}
