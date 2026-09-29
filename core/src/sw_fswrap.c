/*
 * sw_fswrap.c — Move filesystem emulation, linked into every module image.
 *
 * Modules were written for Ableton Move, where user content lives under
 * /data/UserData. On a desktop that path does not exist, so a path beginning
 * with "/data/UserData" is rewritten onto $SCHWUNG_DATA_ROOT; every other path
 * passes through untouched.
 *
 * HOW. The toolchain shim (tools/toolchain/xcc.py) links this object into
 * every shared object it produces. It DEFINES fopen, open, stat, ... itself,
 * with hidden visibility: the static linker binds the module's own references
 * to these local definitions, nothing is exported, nothing interposes on any
 * other image, and the real libc entry points are reached with
 * dlsym(RTLD_NEXT, ...). Because this happens at symbol level, source code is
 * untouched: struct members named `fopen` (FluidLite's file API), C++'s
 * std::remove algorithm, fstream::open, a function the module itself calls
 * `close` — none of them are affected, unlike a preprocessor approach.
 *
 * Unset SCHWUNG_DATA_ROOT -> identity, so the same binaries behave on a Move.
 */
#if defined(__linux__)
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define SW_HIDDEN __attribute__((visibility("hidden")))
#define SW_PREFIX "/data/UserData"

static const char *sw_map(const char *p, char *buf, size_t n) {
    if (!p) return p;
    const size_t pl = sizeof(SW_PREFIX) - 1;
    if (strncmp(p, SW_PREFIX, pl) != 0 || (p[pl] != '\0' && p[pl] != '/')) return p;
    const char *root = getenv("SCHWUNG_DATA_ROOT");
    if (!root || !root[0]) return p;
    snprintf(buf, n, "%s%s", root, p + pl);
    return buf;
}

/* Resolve the next definition once; RTLD_NEXT from inside a dlopen()'d image
 * finds libc (or whatever the process would otherwise have used). */
/* On Intel macOS several libc entry points are bound under suffixed names
 * (stat -> stat$INODE64). Our definitions inherit the same asm labels from the
 * system headers, so the module's references match; the REAL lookup must ask
 * for the suffixed name too. */
#if defined(__APPLE__) && defined(__x86_64__)
static const char *sw_sym(const char *n) {
    static const char *const map[][2] = {
        {"stat", "stat$INODE64"}, {"lstat", "lstat$INODE64"}, {"opendir", "opendir$INODE64"},
        {"scandir", "scandir$INODE64"}, {"realpath", "realpath$DARWIN_EXTSN"},
        {"fopen", "fopen$DARWIN_EXTSN"}};
    for (size_t i = 0; i < sizeof map / sizeof map[0]; ++i)
        if (strcmp(n, map[i][0]) == 0) return map[i][1];
    return n;
}
#else
#define sw_sym(n) (n)
#endif

static void *sw_real(const char *name) {
    void *f = dlsym(RTLD_NEXT, sw_sym(name));
    return f ? f : dlsym(RTLD_NEXT, name);
}

#define REAL(ret, name, args) \
    static ret (*real_##name) args; \
    if (!real_##name) real_##name = (ret (*) args)sw_real(#name)

#define MAP1(p) char b1[PATH_MAX]; const char *m1 = sw_map((p), b1, sizeof b1)

SW_HIDDEN FILE *fopen(const char *p, const char *m) {
    REAL(FILE *, fopen, (const char *, const char *)); MAP1(p); return real_fopen(m1, m);
}
SW_HIDDEN FILE *freopen(const char *p, const char *m, FILE *f) {
    REAL(FILE *, freopen, (const char *, const char *, FILE *)); MAP1(p); return real_freopen(m1, m, f);
}
SW_HIDDEN int open(const char *p, int flags, ...) {
    REAL(int, open, (const char *, int, ...));
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }
    MAP1(p); return real_open(m1, flags, mode);
}
SW_HIDDEN DIR *opendir(const char *p) { REAL(DIR *, opendir, (const char *)); MAP1(p); return real_opendir(m1); }
SW_HIDDEN int stat(const char *p, struct stat *s) { REAL(int, stat, (const char *, struct stat *)); MAP1(p); return real_stat(m1, s); }
SW_HIDDEN int lstat(const char *p, struct stat *s) { REAL(int, lstat, (const char *, struct stat *)); MAP1(p); return real_lstat(m1, s); }
SW_HIDDEN int access(const char *p, int m) { REAL(int, access, (const char *, int)); MAP1(p); return real_access(m1, m); }
SW_HIDDEN int mkdir(const char *p, mode_t m) { REAL(int, mkdir, (const char *, mode_t)); MAP1(p); return real_mkdir(m1, m); }
SW_HIDDEN int rmdir(const char *p) { REAL(int, rmdir, (const char *)); MAP1(p); return real_rmdir(m1); }
SW_HIDDEN int unlink(const char *p) { REAL(int, unlink, (const char *)); MAP1(p); return real_unlink(m1); }
SW_HIDDEN int remove(const char *p) { REAL(int, remove, (const char *)); MAP1(p); return real_remove(m1); }
SW_HIDDEN int chdir(const char *p) { REAL(int, chdir, (const char *)); MAP1(p); return real_chdir(m1); }
SW_HIDDEN char *realpath(const char *p, char *r) { REAL(char *, realpath, (const char *, char *)); MAP1(p); return real_realpath(m1, r); }
SW_HIDDEN int rename(const char *a, const char *b) {
    REAL(int, rename, (const char *, const char *));
    char ba[PATH_MAX], bb[PATH_MAX];
    return real_rename(sw_map(a, ba, sizeof ba), sw_map(b, bb, sizeof bb));
}
SW_HIDDEN int scandir(const char *p, struct dirent ***nl, int (*f)(const struct dirent *),
                      int (*c)(const struct dirent **, const struct dirent **)) {
    REAL(int, scandir, (const char *, struct dirent ***, int (*)(const struct dirent *),
                        int (*)(const struct dirent **, const struct dirent **)));
    MAP1(p); return real_scandir(m1, nl, f, c);
}

#if defined(__linux__) && defined(__GLIBC__)
/* glibc: code built with _FILE_OFFSET_BITS=64 (or C++ on some distros)
 * references the *64 names instead. */
SW_HIDDEN FILE *fopen64(const char *p, const char *m) {
    REAL(FILE *, fopen64, (const char *, const char *)); MAP1(p); return real_fopen64(m1, m);
}
SW_HIDDEN int open64(const char *p, int flags, ...) {
    REAL(int, open64, (const char *, int, ...));
    mode_t mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }
    MAP1(p); return real_open64(m1, flags, mode);
}
SW_HIDDEN DIR *opendir64(const char *p) { return opendir(p); }
SW_HIDDEN int stat64(const char *p, struct stat64 *s) { REAL(int, stat64, (const char *, struct stat64 *)); MAP1(p); return real_stat64(m1, s); }
SW_HIDDEN int lstat64(const char *p, struct stat64 *s) { REAL(int, lstat64, (const char *, struct stat64 *)); MAP1(p); return real_lstat64(m1, s); }
SW_HIDDEN int scandir64(const char *p, struct dirent64 ***nl, int (*f)(const struct dirent64 *),
                        int (*c)(const struct dirent64 **, const struct dirent64 **)) {
    REAL(int, scandir64, (const char *, struct dirent64 ***, int (*)(const struct dirent64 *),
                          int (*)(const struct dirent64 **, const struct dirent64 **)));
    MAP1(p); return real_scandir64(m1, nl, f, c);
}
#endif
