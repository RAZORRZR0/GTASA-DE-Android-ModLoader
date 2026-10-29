// Android glue for the Win32 calls the PC dllmain.cpp used. Keeps the mod body unchanged.
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <android/log.h>

typedef uint32_t DWORD;
typedef int32_t LONG;
typedef int BOOL;
typedef void* HANDLE;
typedef void* HMODULE;
typedef void* LPVOID;
typedef pthread_mutex_t SRWLOCK;

#define MAX_PATH PATH_MAX
#define WINAPI
#define APIENTRY
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define SRWLOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define _SH_DENYWR 0


#define InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_RELAXED)
#define InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_RELAXED)

static inline DWORD GetTickCount(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
static inline DWORD GetCurrentThreadId(void) { return (DWORD)gettid(); }
static inline void Sleep(DWORD ms) { usleep(ms * 1000u); }
static inline void AcquireSRWLockExclusive(SRWLOCK* l) { pthread_mutex_lock(l); }
static inline void ReleaseSRWLockExclusive(SRWLOCK* l) { pthread_mutex_unlock(l); }
static inline void OutputDebugStringA(const char* s) { __android_log_print(ANDROID_LOG_INFO, "GTAPropFix", "%s", s); }
static inline FILE* _fsopen(const char* path, const char* mode, int) { return fopen(path, mode); }
static inline void CreateDirectoryA(const char* path, void*) { mkdir(path, 0770); }
template <size_t N> static inline void strcpy_s(char (&d)[N], const char* s) { snprintf(d, N, "%s", s); }
static inline void strcpy_s(char* d, size_t cap, const char* s) { snprintf(d, cap, "%s", s); }
template <size_t N> static inline void strcat_s(char (&d)[N], const char* s) { strncat(d, s, N - strlen(d) - 1); }

// INI reader used by LoadConfig. Section and key match is case-insensitive, as in Win32.
static inline bool IniLookup(const char* path, const char* section, const char* key, char* out, size_t outLen) {
    out[0] = '\0';
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[512], cur[128] = "";
    bool found = false;
    while (!found && fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '[') {
            char* e = strchr(p, ']');
            if (!e) continue;
            size_t n = (size_t)(e - p - 1);
            if (n >= sizeof(cur)) n = sizeof(cur) - 1;
            memcpy(cur, p + 1, n);
            cur[n] = '\0';
            continue;
        }
        if (strcasecmp(cur, section) != 0) continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        char* k = p;
        char* ke = eq;
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) --ke;
        if ((size_t)(ke - k) != strlen(key) || strncasecmp(k, key, (size_t)(ke - k)) != 0) continue;
        char* v = eq + 1;
        while (*v == ' ' || *v == '\t') ++v;
        char* ve = v + strlen(v);
        while (ve > v && (ve[-1] == '\n' || ve[-1] == '\r' || ve[-1] == ' ' || ve[-1] == '\t')) --ve;
        size_t n = (size_t)(ve - v);
        if (n >= outLen) n = outLen - 1;
        memcpy(out, v, n);
        out[n] = '\0';
        found = true;
    }
    fclose(f);
    return found;
}
static inline DWORD GetPrivateProfileStringA(const char* section, const char* key, const char* def, char* out, DWORD outLen, const char* path) {
    if (!IniLookup(path, section, key, out, outLen)) snprintf(out, outLen, "%s", def);
    return (DWORD)strlen(out);
}
static inline int GetPrivateProfileIntA(const char* section, const char* key, int def, const char* path) {
    char buf[32];
    if (!IniLookup(path, section, key, buf, sizeof(buf))) return def;
    return (int)strtol(buf, nullptr, 0);
}
