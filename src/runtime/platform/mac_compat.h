// What the Windows-first port uses that macOS spells differently (included via runtime/platform.h on macOS only).
#pragma once
#include <errno.h>
#include <stdlib.h>
#include <time.h>

#define O_BINARY 0                  // (no text mode)
#define LOCALE_NAME_MAX_LENGTH 85   // a BCP 47 tag, as Windows sizes it

static inline int localtime_s(struct tm *out, const time_t *t) { return localtime_r(t, out) ? 0 : EINVAL; }
static inline int gmtime_s(struct tm *out, const time_t *t) { return gmtime_r(t, out) ? 0 : EINVAL; }

static inline void *_aligned_malloc(size_t size, size_t align) {
    void *p = NULL;
    return posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, size) ? NULL : p;
}
static inline void _aligned_free(void *p) { free(p); }

#include <unistd.h>
static inline int _chsize_s(int fd, long long size) { return ftruncate(fd, (off_t)size) ? errno : 0; }
static inline time_t _mkgmtime(struct tm *tm) { return timegm(tm); }
#include <stdio.h>
#define _fseeki64 fseeko   // (off_t is 64-bit on macOS)
#define _ftelli64 ftello
