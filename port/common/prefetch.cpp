#include "prefetch.h"
#include "disc.h"
#include <stdlib.h>
#include <string.h>

namespace Prefetch {

unsigned statHit = 0, statMiss = 0, statLoaded = 0;
unsigned long long statBytesLoaded = 0;

static const int MAX_QUEUE = 160;
static const int MAX_STORE = 96;
static const size_t BUDGET = 3u * 1024 * 1024;        /* octets compressés conservés au maximum */
static const unsigned RESERVE_KB = 2048;              /* mémoire à laisser libre pour le jeu */

struct Item { uint32_t id; uint8_t *blob; uint32_t csize; };

static Env s_env = { nullptr, nullptr, nullptr };
static const uint32_t *s_tbl = nullptr;
static uint32_t s_count = 0;
static Disc::File *s_file = nullptr;

static uint32_t s_queue[MAX_QUEUE]; static int s_qn = 0, s_qi = 0;
static Item s_store[MAX_STORE]; static int s_sn = 0;
static size_t s_bytes = 0;
static struct { uint32_t id; uint8_t *buf; uint32_t size, got; } s_cur = { 0, nullptr, 0, 0 };

static void lock() { if (s_env.lock) s_env.lock(); }
static void unlock() { if (s_env.unlock) s_env.unlock(); }

void setEnv(const Env &e) { s_env = e; }

void configure(const char *packName, const uint32_t *table, uint32_t count) {
	shutdown();
	s_tbl = table; s_count = count;
	s_file = Disc::open(packName, true);
}

void shutdown() {
	lock();
	for (int i = 0; i < s_sn; i++) free(s_store[i].blob);
	s_sn = 0; s_bytes = 0; s_qn = s_qi = 0;
	free(s_cur.buf); s_cur.buf = nullptr;
	unlock();
	if (s_file) { Disc::close(s_file); s_file = nullptr; }
	s_tbl = nullptr; s_count = 0;
}

static int findStore(uint32_t id) {
	for (int i = 0; i < s_sn; i++) if (s_store[i].id == id) return i;
	return -1;
}

bool has(uint32_t id) {
	lock();
	bool r = findStore(id) >= 0;
	unlock();
	return r;
}

static bool wanted(uint32_t id) {
	for (int i = 0; i < s_qn; i++) if (s_queue[i] == id) return true;
	return false;
}

void request(const uint32_t *ids, uint32_t n) {
	if (!s_file) return;
	lock();
	s_qn = 0; s_qi = 0;
	for (uint32_t i = 0; i < n && s_qn < MAX_QUEUE; i++) {
		if (ids[i] >= s_count || s_tbl[ids[i] * 4 + 1] == 0) continue;
		if (wanted(ids[i])) continue;
		s_queue[s_qn++] = ids[i];
	}
	/* ce qui a été lu mais n'est plus demandé est libéré */
	for (int i = 0; i < s_sn;) {
		if (!wanted(s_store[i].id)) {
			s_bytes -= s_store[i].csize; free(s_store[i].blob);
			s_store[i] = s_store[--s_sn];
		} else i++;
	}
	unlock();
}

uint8_t *take(uint32_t id, uint32_t *csize) {
	lock();
	int i = findStore(id);
	uint8_t *b = nullptr;
	if (i >= 0) {
		b = s_store[i].blob; if (csize) *csize = s_store[i].csize;
		s_bytes -= s_store[i].csize;
		s_store[i] = s_store[--s_sn];
		statHit++;
	} else if (s_count) statMiss++;
	unlock();
	return b;
}

void flush() {
	lock();
	for (int i = 0; i < s_sn; i++) free(s_store[i].blob);
	s_sn = 0; s_bytes = 0; s_qn = s_qi = 0;
	unlock();
}

size_t storedBytes() { return s_bytes; }

bool pump() {
	if (!s_file || !s_tbl) return false;
	if (!s_cur.buf) {
		lock();
		uint32_t id = 0; bool found = false;
		while (s_qi < s_qn) {
			uint32_t cand = s_queue[s_qi];
			if (findStore(cand) >= 0) { s_qi++; continue; }
			id = cand; found = true; break;
		}
		if (found) {
			uint32_t cs = s_tbl[id * 4 + 1];
			bool room = s_sn < MAX_STORE && s_bytes + cs <= BUDGET &&
			            (!s_env.freeKB || s_env.freeKB() >= RESERVE_KB + cs / 1024);
			if (!room) found = false;
			else {
				uint8_t *b = (uint8_t *)malloc(cs);
				if (!b) found = false;
				else { s_cur.id = id; s_cur.buf = b; s_cur.size = cs; s_cur.got = 0; s_qi++; }
			}
		}
		unlock();
		if (!found) return false;
	}
	uint32_t off = s_tbl[s_cur.id * 4];
	uint32_t n = s_cur.size - s_cur.got;
	if (n > 32 * 1024) n = 32 * 1024;
	long r = Disc::readAt(s_file, off + s_cur.got, s_cur.buf + s_cur.got, n);
	if (r <= 0) { free(s_cur.buf); s_cur.buf = nullptr; return true; }
	s_cur.got += (uint32_t)r;
	if (s_cur.got >= s_cur.size) {
		lock();
		if (s_sn < MAX_STORE && wanted(s_cur.id)) {
			s_store[s_sn].id = s_cur.id; s_store[s_sn].blob = s_cur.buf; s_store[s_sn].csize = s_cur.size; s_sn++;
			s_bytes += s_cur.size; statLoaded++; statBytesLoaded += s_cur.size;
		} else free(s_cur.buf);
		s_cur.buf = nullptr;
		unlock();
	}
	return true;
}

}
