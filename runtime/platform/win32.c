// Windows implementation of platform.h: CreateThread, QueryPerformanceCounter, high-resolution waitable timers,
// SRW locks and condition variables.
#include <stdio.h>
#include <windows.h>
#include <bcrypt.h>
#include <time.h>
#include <timeapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <io.h>
#include <psapi.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "platform.h"

_Static_assert(sizeof(PlatMutex) == sizeof(SRWLOCK), "PlatMutex holds an SRWLOCK");
_Static_assert(sizeof(PlatCond) == sizeof(CONDITION_VARIABLE), "PlatCond holds a CONDITION_VARIABLE");

// ---- threads

struct PlatThread {
    HANDLE h;
    PlatThreadFn fn;
    void *arg;
};

static DWORD WINAPI thread_main(LPVOID p) {
    PlatThread *t = p;
    t->fn(t->arg);
    return 0;
}

PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    return plat_thread_new_stack(fn, arg, name, 0);
}

PlatThread *plat_thread_new_stack(PlatThreadFn fn, void *arg, const char *name, size_t stack_bytes) {
    PlatThread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    t->h = CreateThread(NULL, stack_bytes, thread_main, t, CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t->h) { free(t); return NULL; }
    if (name) {
        wchar_t w[64];
        if (MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 64)) SetThreadDescription(t->h, w);
    }
    ResumeThread(t->h);
    return t;
}

void plat_thread_join(PlatThread *t) {
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

// The thread keeps using t->fn and t->arg, so the struct stays allocated (a few bytes per detached thread).
void plat_thread_detach(PlatThread *t) {
    CloseHandle(t->h);
    t->h = NULL;
}

uint64_t plat_thread_affinity(PlatThread *t, uint64_t mask) {
    return (uint64_t)SetThreadAffinityMask(t ? t->h : GetCurrentThread(), (DWORD_PTR)mask);
}

uint32_t plat_cpu_count(void) {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n ? (uint32_t)n : 1;
}

void plat_thread_name(const char *name) {
    wchar_t w[64];
    if (MultiByteToWideChar(CP_UTF8, 0, name, -1, w, 64)) SetThreadDescription(GetCurrentThread(), w);
}

void plat_thread_priority(PlatThread *t, int level) { SetThreadPriority(t ? t->h : GetCurrentThread(), level); }

uint64_t plat_thread_cpu_ns(uint32_t tid) {
    HANDLE th = tid ? OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid) : NULL;
    FILETIME a, b, k, u;
    uint64_t ns = 0;
    if (th && GetThreadTimes(th, &a, &b, &k, &u))
        ns = (((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime)) * 100;
    if (th) CloseHandle(th);
    return ns;
}

static PlatInterruptFn g_interrupt;

void plat_interrupt_init(PlatInterruptFn fn) { g_interrupt = fn; }

PlatThreadRef *plat_thread_ref_self(void) {
    return (PlatThreadRef *)OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                                       FALSE, GetCurrentThreadId());
}

void plat_thread_ref_close(PlatThreadRef *r) { if (r) CloseHandle((HANDLE)r); }

int plat_thread_interrupt(PlatThreadRef *r, void *arg) {
    HANDLE h = (HANDLE)r;
    if (SuspendThread(h) == (DWORD)-1) return PLAT_INT_FAILED;
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;   // set back whole, so the kernel restores every GPR
    int res = PLAT_INT_FAILED;
    if (GetThreadContext(h, &ctx)) {
        uint64_t to = g_interrupt(ctx.Rip, arg);
        res = PLAT_INT_LEFT;
        if (to) {
            ctx.Rip = to;
            res = SetThreadContext(h, &ctx) ? PLAT_INT_REDIRECTED : PLAT_INT_FAILED;
        }
    }
    ResumeThread(h);
    return res;
}

PlatFiber *plat_fiber_convert(void) { return (PlatFiber *)ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH); }
void plat_fiber_unconvert(void) { ConvertFiberToThread(); }

PlatFiber *plat_fiber_new(size_t stack_bytes, void (*fn)(void *arg), void *arg) {
    return (PlatFiber *)CreateFiberEx(0x10000, stack_bytes, FIBER_FLAG_FLOAT_SWITCH, (LPFIBER_START_ROUTINE)(void *)fn, arg);
}

void plat_fiber_switch(PlatFiber *to) { SwitchToFiber(to); }
void plat_fiber_delete(PlatFiber *f) { DeleteFiber(f); }

int plat_thread_priority_get(void) { return GetThreadPriority(GetCurrentThread()); }
int plat_thread_wait_ms(PlatThread *t, uint32_t ms) { return WaitForSingleObject(t->h, ms) != WAIT_TIMEOUT; }

void plat_yield(void) { SwitchToThread(); }

// ---- time

static uint64_t qpc_freq(void) {
    static uint64_t freq;
    if (!freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = (uint64_t)f.QuadPart;
    }
    return freq;
}

static PlatScale qpc_ns;   // set before main, so no thread reads it half written

__attribute__((constructor)) static void qpc_ns_init(void) { qpc_ns = plat_scale(1000000000u, qpc_freq()); }

uint64_t plat_now_ns(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return plat_scaled(qpc_ns, (uint64_t)t.QuadPart);
}

// A high-resolution waitable timer per sleeping thread (Windows 10 1803+; one handle per thread, kept until exit)
// wakes within about 0.1-0.5 ms; the last PLAT_SPIN_NS are spent yielding so the deadline is met closely.
// Without that timer kind, Sleep(1) granularity applies.
#define PLAT_SPIN_NS 300000u

static __thread HANDLE sleep_timer;
static __thread int sleep_timer_tried;

void plat_sleep_ns(uint64_t ns) {
    uint64_t end = plat_now_ns() + ns;
    if (!sleep_timer_tried) {
        sleep_timer_tried = 1;
        sleep_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    if (ns > PLAT_SPIN_NS) {
        uint64_t coarse = ns - PLAT_SPIN_NS;
        if (sleep_timer) {
            LARGE_INTEGER due = {.QuadPart = -(LONGLONG)(coarse / 100)};   // relative, 100 ns units
            if (SetWaitableTimerEx(sleep_timer, &due, 0, NULL, NULL, NULL, 0))
                WaitForSingleObject(sleep_timer, INFINITE);
            else Sleep((DWORD)(coarse / 1000000));
        } else Sleep((DWORD)(coarse / 1000000));
    }
    while (plat_now_ns() < end) SwitchToThread();
}

void plat_wait_ns(uint64_t ns) {
    if (!sleep_timer_tried) {
        sleep_timer_tried = 1;
        sleep_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    LARGE_INTEGER due = {.QuadPart = -(LONGLONG)(ns / 100 + 1)};   // relative, 100 ns units, rounded up
    if (sleep_timer && SetWaitableTimerEx(sleep_timer, &due, 0, NULL, NULL, NULL, 0)) WaitForSingleObject(sleep_timer, INFINITE);
    else Sleep((DWORD)(ns / 1000000));
}

void plat_sleep_ms(uint32_t ms) { Sleep(ms); }
uint64_t plat_counter(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}
uint64_t plat_counter_hz(void) { return qpc_freq(); }
void plat_timer_fine(void) { timeBeginPeriod(1); }
uint64_t plat_tick_ms(void) { return GetTickCount64(); }

// ---- locks and condition variables

void plat_mutex_init(PlatMutex *m) { InitializeSRWLock((PSRWLOCK)m); }
void plat_mutex_lock(PlatMutex *m) { AcquireSRWLockExclusive((PSRWLOCK)m); }
int plat_mutex_trylock(PlatMutex *m) { return TryAcquireSRWLockExclusive((PSRWLOCK)m) != 0; }
void plat_mutex_unlock(PlatMutex *m) { ReleaseSRWLockExclusive((PSRWLOCK)m); }

void plat_mutex_lock_shared(PlatMutex *m) { AcquireSRWLockShared((PSRWLOCK)m); }
int plat_mutex_trylock_shared(PlatMutex *m) { return TryAcquireSRWLockShared((PSRWLOCK)m) != 0; }
void plat_mutex_unlock_shared(PlatMutex *m) { ReleaseSRWLockShared((PSRWLOCK)m); }

void plat_cond_init(PlatCond *c) { InitializeConditionVariable((PCONDITION_VARIABLE)c); }
void plat_cond_wait(PlatCond *c, PlatMutex *m) { SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, INFINITE, 0); }

int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
    uint64_t ms = (ns + 999999) / 1000000;
    DWORD to = ms >= INFINITE ? INFINITE - 1 : (DWORD)ms;
    return SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, to, 0) ? 1 : GetLastError() != ERROR_TIMEOUT;
}

void plat_cond_signal(PlatCond *c) { WakeConditionVariable((PCONDITION_VARIABLE)c); }
void plat_cond_broadcast(PlatCond *c) { WakeAllConditionVariable((PCONDITION_VARIABLE)c); }

_Static_assert(sizeof(PlatOnce) == sizeof(INIT_ONCE), "PlatOnce holds an INIT_ONCE");

static BOOL CALLBACK once_call(INIT_ONCE *o, void *fn, void **ctx) {
    (void)o, (void)ctx;
    ((void (*)(void))fn)();
    return TRUE;
}

void plat_once(PlatOnce *o, void (*fn)(void)) { InitOnceExecuteOnce((INIT_ONCE *)o, once_call, (void *)fn, NULL); }

PlatEvent *plat_event_new(int manual, int initially_set) {
    return (PlatEvent *)CreateEventW(NULL, manual, initially_set, NULL);
}
void plat_event_free(PlatEvent *e) { if (e) CloseHandle((HANDLE)e); }
void plat_event_set(PlatEvent *e) { SetEvent((HANDLE)e); }
void plat_event_reset(PlatEvent *e) { ResetEvent((HANDLE)e); }
void plat_event_wait(PlatEvent *e) { WaitForSingleObject((HANDLE)e, INFINITE); }
int plat_event_wait_ms(PlatEvent *e, uint32_t ms) { return WaitForSingleObject((HANDLE)e, ms) == WAIT_OBJECT_0; }

// ---- files

static int widen(const char *s, wchar_t *w, int cap) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap);
    if (!n) w[0] = 0;
    return n;
}

#define WPATH 2048

int plat_file_exists(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void plat_file_mkdir_p(const char *path) {
    wchar_t tmp[WPATH];
    if (!widen(path, tmp, WPATH)) return;
    for (wchar_t *p = tmp + 1; *p; p++) {
        if (*p == L'\\' || *p == L'/') {
            wchar_t c = *p;
            *p = 0;
            CreateDirectoryW(tmp, NULL);
            *p = c;
        }
    }
    CreateDirectoryW(tmp, NULL);
}

uint8_t *plat_file_read(const char *path, uint64_t *size) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    uint8_t *buf = NULL;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < (1ll << 30)) {
        buf = malloc((size_t)sz.QuadPart + 1);
        DWORD got = 0;
        if (buf && (!ReadFile(h, buf, (DWORD)sz.QuadPart, &got, NULL) || got != sz.QuadPart)) {
            free(buf);
            buf = NULL;
        }
        if (buf) {
            buf[sz.QuadPart] = 0;
            *size = (uint64_t)sz.QuadPart;
        }
    }
    CloseHandle(h);
    return buf;
}

int plat_file_write(const char *path, const void *data, size_t n) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    int ok = WriteFile(h, data, (DWORD)n, &put, NULL) && put == n;
    CloseHandle(h);
    return ok;
}

PlatAppend *plat_append_open(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return NULL;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    return h == INVALID_HANDLE_VALUE ? NULL : (PlatAppend *)h;
}

int plat_append_write(PlatAppend *f, const void *data, size_t n) {
    // Offset and OffsetHigh all ones: WriteFile appends at the end of the file, atomically against other writers.
    OVERLAPPED o = {.Offset = 0xffffffff, .OffsetHigh = 0xffffffff};
    DWORD put = 0;
    return WriteFile((HANDLE)f, data, (DWORD)n, &put, &o) && put == n;
}

int plat_file_replace(const char *from, const char *to) {
    wchar_t a[WPATH], b[WPATH];
    if (!widen(from, a, WPATH) || !widen(to, b, WPATH)) return 0;
    return MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

int plat_file_write_atomic(const char *path, const void *data, size_t n) {
    char tmp[WPATH + 48];
    wchar_t a[WPATH], b[WPATH];
    int len = snprintf(tmp, sizeof tmp, "%s.%lu.%lu.tmp", path, GetCurrentProcessId(), GetCurrentThreadId());
    if (len < 0 || len >= (int)sizeof tmp || !widen(tmp, a, WPATH) || !widen(path, b, WPATH)) return 0;
    HANDLE h = CreateFileW(a, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD put = 0;
    int ok = WriteFile(h, data, (DWORD)n, &put, NULL) && put == n && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return 1;
    DeleteFileW(a);
    return 0;
}

int plat_file_flush(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    return ok;
}

int plat_file_delete(const char *path) {
    wchar_t w[WPATH];
    return widen(path, w, WPATH) && DeleteFileW(w);
}

int plat_file_truncate(const char *path, uint64_t size) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER at = {.QuadPart = (LONGLONG)size};
    int ok = SetFilePointerEx(h, at, NULL, FILE_BEGIN) && SetEndOfFile(h);
    CloseHandle(h);
    return ok;
}

int64_t plat_file_size(const char *path) {
    wchar_t w[WPATH];
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!widen(path, w, WPATH) || !GetFileAttributesExW(w, GetFileExInfoStandard, &d)) return -1;
    return (int64_t)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow);
}

int plat_file_set_readonly(const char *path, int on) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    return SetFileAttributesW(w, on ? a | FILE_ATTRIBUTE_READONLY : a & ~(DWORD)FILE_ATTRIBUTE_READONLY) != 0;
}

int plat_file_list(const char *dir, const char *ext, int (*cb)(const char *name, void *ctx), void *ctx) {
    wchar_t wd[WPATH], we[64], pat[WPATH + 64];
    if (!widen(dir, wd, WPATH) || !widen(ext, we, 64)) return 0;
    swprintf(pat, WPATH + 64, L"%ls\\*%ls", wd, we);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char name[1024];
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
        if (!cb(name, ctx)) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 1;
}

FILE *plat_fopen(const char *path, const char *mode) {
    wchar_t w[WPATH], m[16];
    if (!widen(path, w, WPATH) || !widen(mode, m, 16)) return NULL;
    return _wfopen(w, m);
}

int plat_file_sync_stream(FILE *f) { return !_commit(_fileno(f)); }
int plat_fseek(FILE *f, int64_t at, int whence) { return _fseeki64(f, at, whence); }
int64_t plat_ftell(FILE *f) { return _ftelli64(f); }

PlatFile *plat_file_open(const char *path, int flags) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return NULL;
    DWORD access = (flags & PLAT_FILE_READ ? GENERIC_READ : 0) | (flags & PLAT_FILE_WRITE ? GENERIC_WRITE : 0);
    DWORD attrs = (flags & PLAT_FILE_UNBUFFERED ? FILE_FLAG_NO_BUFFERING : FILE_ATTRIBUTE_NORMAL) |
                  (flags & PLAT_FILE_PARALLEL ? FILE_FLAG_OVERLAPPED : 0);
    DWORD create = flags & PLAT_FILE_EXCL ? CREATE_NEW : flags & PLAT_FILE_TRUNC ? (flags & PLAT_FILE_CREATE ? CREATE_ALWAYS : TRUNCATE_EXISTING)
                                           : flags & PLAT_FILE_CREATE ? OPEN_ALWAYS : OPEN_EXISTING;
    HANDLE h = CreateFileW(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, create, attrs, NULL);
    return h == INVALID_HANDLE_VALUE ? NULL : (PlatFile *)h;
}

void plat_file_close(PlatFile *f) { if (f) CloseHandle((HANDLE)f); }

// Works on synchronous and FILE_FLAG_OVERLAPPED handles; the event is per thread, so concurrent reads on one handle do
// not share it.
int64_t plat_file_pread(PlatFile *f, void *buf, uint64_t n, uint64_t at) {
    static _Thread_local HANDLE ev;
    if (!ev && !(ev = CreateEventW(NULL, TRUE, FALSE, NULL))) return -1;
    uint64_t done = 0;
    while (done < n) {
        uint64_t off = at + done;
        OVERLAPPED o = {.Offset = (DWORD)off, .OffsetHigh = (DWORD)(off >> 32), .hEvent = ev};
        DWORD want = n - done > 0x40000000u ? 0x40000000u : (DWORD)(n - done), got = 0;
        if (!ReadFile((HANDLE)f, (char *)buf + done, want, &got, &o)) {
            DWORD e = GetLastError();
            if (e == ERROR_IO_PENDING && !GetOverlappedResult((HANDLE)f, &o, &got, TRUE)) e = GetLastError();
            else if (e == ERROR_IO_PENDING) e = 0;
            if (e == ERROR_HANDLE_EOF) break;
            if (e) {
                SetLastError(e);
                return -1;
            }
        }
        if (!got) break;
        done += got;
    }
    return (int64_t)done;
}

int64_t plat_file_pwrite(PlatFile *f, const void *buf, uint64_t n, uint64_t at) {
    uint64_t done = 0;
    while (done < n) {
        uint64_t off = at + done;
        OVERLAPPED o = {.Offset = (DWORD)off, .OffsetHigh = (DWORD)(off >> 32)};
        DWORD want = n - done > 0x40000000u ? 0x40000000u : (DWORD)(n - done), put = 0;
        if (!WriteFile((HANDLE)f, (const char *)buf + done, want, &put, &o) || !put) return -1;
        done += put;
    }
    return (int64_t)done;
}

int64_t plat_file_handle_size(PlatFile *f) {
    LARGE_INTEGER sz;
    return GetFileSizeEx((HANDLE)f, &sz) ? (int64_t)sz.QuadPart : -1;
}

int plat_file_set_size(PlatFile *f, uint64_t n) {
    FILE_END_OF_FILE_INFO eof = {.EndOfFile.QuadPart = (LONGLONG)n};
    return SetFileInformationByHandle((HANDLE)f, FileEndOfFileInfo, &eof, sizeof eof) != 0;
}

int plat_file_sync(PlatFile *f) { return FlushFileBuffers((HANDLE)f) != 0; }

int plat_file_identity(PlatFile *f, PlatFileId *id) {
    BY_HANDLE_FILE_INFORMATION i;
    if (!GetFileInformationByHandle((HANDLE)f, &i)) return 0;
    *id = (PlatFileId){i.dwVolumeSerialNumber, 0, (uint64_t)i.nFileIndexHigh << 32 | i.nFileIndexLow,
                       (uint64_t)i.nFileSizeHigh << 32 | i.nFileSizeLow,
                       (uint64_t)i.ftLastWriteTime.dwHighDateTime << 32 | i.ftLastWriteTime.dwLowDateTime,
                       (uint64_t)i.ftCreationTime.dwHighDateTime << 32 | i.ftCreationTime.dwLowDateTime};
    return 1;
}

const void *plat_file_map(PlatFile *f) {
    HANDLE map = CreateFileMappingW((HANDLE)f, NULL, PAGE_READONLY, 0, 0, NULL);
    const void *v = map ? MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (map) CloseHandle(map);   // the view keeps the mapping alive
    return v;
}

int plat_file_links(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    BY_HANDLE_FILE_INFORMATION fi;
    int links = f != INVALID_HANDLE_VALUE && GetFileInformationByHandle(f, &fi) ? (int)fi.nNumberOfLinks : 0;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    return links;
}

int plat_dir_exists(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int plat_dir_remove(const char *path) {
    wchar_t w[WPATH];
    return widen(path, w, WPATH) && RemoveDirectoryW(w);
}

int plat_file_move(const char *from, const char *to) {
    wchar_t a[WPATH], b[WPATH];
    return widen(from, a, WPATH) && widen(to, b, WPATH) && MoveFileExW(a, b, 0);
}

int plat_file_move_durable(const char *from, const char *to) {
    wchar_t a[WPATH], b[WPATH];
    return widen(from, a, WPATH) && widen(to, b, WPATH) && MoveFileExW(a, b, MOVEFILE_WRITE_THROUGH);
}

int plat_file_link(const char *existing, const char *link) {
    wchar_t a[WPATH], b[WPATH];
    return widen(existing, a, WPATH) && widen(link, b, WPATH) && CreateHardLinkW(b, a, NULL);
}

int plat_dir_sync(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    return ok;
}

int plat_path_info(const char *path) {
    wchar_t w[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES) return 0;
    return PLAT_PATH_EXISTS | (a & FILE_ATTRIBUTE_DIRECTORY ? PLAT_PATH_DIR : 0) |
           (a & FILE_ATTRIBUTE_REPARSE_POINT ? PLAT_PATH_LINK : 0) | (a & FILE_ATTRIBUTE_READONLY ? PLAT_PATH_READONLY : 0);
}

int plat_error_kind(uint32_t error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_DIRECTORY: return PLAT_ERR_NOT_FOUND;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return PLAT_ERR_EXISTS;
    case ERROR_DIR_NOT_EMPTY: return PLAT_ERR_NOT_EMPTY;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: return PLAT_ERR_BUSY;
    case ERROR_NOACCESS: return PLAT_ERR_BAD_BUFFER;
    default: return PLAT_ERR_OTHER;
    }
}

int plat_dir_make(const char *path) {
    wchar_t w[WPATH];
    return widen(path, w, WPATH) && (CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
}

int plat_dir_create(const char *path) {
    wchar_t w[WPATH];
    return widen(path, w, WPATH) && CreateDirectoryW(w, NULL);
}

struct PlatDir {
    HANDLE h;
    WIN32_FIND_DATAW fd;
    int have;
    char name[1024];
};

PlatDir *plat_dir_open(const char *dir) {
    wchar_t wd[WPATH], pat[WPATH + 8];
    if (!widen(dir, wd, WPATH)) return NULL;
    swprintf(pat, WPATH + 8, L"%ls\\*", wd);
    PlatDir *d = malloc(sizeof *d);
    if (!d) return NULL;
    d->h = FindFirstFileW(pat, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        free(d);
        SetLastError(e);
        return NULL;
    }
    d->have = 1;
    return d;
}

int plat_dir_next(PlatDir *d, PlatDirEntry *e) {
    for (;;) {
        if (!d->have && !FindNextFileW(d->h, &d->fd)) return 0;
        d->have = 0;
        const WIN32_FIND_DATAW *f = &d->fd;
        if (!wcscmp(f->cFileName, L".") || !wcscmp(f->cFileName, L"..")) continue;
        if (!WideCharToMultiByte(CP_UTF8, 0, f->cFileName, -1, d->name, sizeof d->name, NULL, NULL)) continue;
        e->name = d->name;
        e->is_dir = (f->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e->is_link = (f->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        e->size = (uint64_t)f->nFileSizeHigh << 32 | f->nFileSizeLow;
        return 1;
    }
}

void plat_dir_close(PlatDir *d) {
    if (!d) return;
    FindClose(d->h);
    free(d);
}

// ---- memory

int plat_mem_reserve(uint64_t addr, size_t n, int watch) {
    void *want = (void *)(uintptr_t)addr;
    void *p = VirtualAlloc(want, n, MEM_RESERVE | (watch ? MEM_WRITE_WATCH : 0), PAGE_NOACCESS);
    if (p && p != want) {
        VirtualFree(p, 0, MEM_RELEASE);
        p = NULL;
    }
    return p != NULL;
}

void plat_mem_release(uint64_t addr, size_t n) { (void)n; VirtualFree((void *)(uintptr_t)addr, 0, MEM_RELEASE); }

int plat_mem_commit(uint64_t addr, size_t n) {
    return VirtualAlloc((void *)(uintptr_t)addr, n, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

int plat_mem_decommit(uint64_t addr, size_t n) { return VirtualFree((void *)(uintptr_t)addr, n, MEM_DECOMMIT) != 0; }

int plat_mem_protect(uint64_t addr, size_t n, int prot) {
    static const DWORD win[] = {PAGE_NOACCESS, PAGE_READONLY, PAGE_READWRITE};
    DWORD old;
    return VirtualProtect((void *)(uintptr_t)addr, n, win[prot], &old) != 0;
}

// Adapted from LAS Recomp runtime/platform/windows.c:40-63 @ da35b83 (MIT, Copyright (c) 2026 Peiikai): GetWriteWatch
// with the reset flag.
uint64_t plat_mem_written(uint64_t addr, size_t n, uint64_t *pages, uint64_t max, int reset) {
    ULONG_PTR count = (ULONG_PTR)max;
    DWORD gran;
    _Static_assert(sizeof(PVOID) == sizeof(uint64_t), "page addresses are written straight into a uint64_t array");
    if (GetWriteWatch(reset ? WRITE_WATCH_FLAG_RESET : 0, (void *)(uintptr_t)addr, (SIZE_T)n, (PVOID *)pages, &count, &gran))
        return UINT64_MAX;
    return count;
}

// Reserved holes (heap pages given back) stay as they are, and plat_mem_thaw finds the read-only runs again.
int plat_mem_can_freeze(void) { return 1; }

int plat_mem_freeze(uint64_t addr, size_t size) {
    uint64_t a = addr, end = a + size, run[16][2];
    int n = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uint64_t x = a; x < end; ) {
        if (!VirtualQuery((void *)(uintptr_t)x, &mbi, sizeof mbi)) return 0;
        uint64_t y = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (y > end) y = end;
        if (mbi.State == MEM_COMMIT) {
            if (mbi.Protect != PAGE_READWRITE || n == 16) return 0;
            run[n][0] = x, run[n][1] = y, n++;
        } else if (mbi.State != MEM_RESERVE) return 0;
        x = y;
    }
    DWORD old;
    for (int i = 0; i < n; i++)
        if (!VirtualProtect((void *)(uintptr_t)run[i][0], run[i][1] - run[i][0], PAGE_READONLY, &old)) {
            while (i--) VirtualProtect((void *)(uintptr_t)run[i][0], run[i][1] - run[i][0], PAGE_READWRITE, &old);
            return 0;
        }
    return n > 0;
}

int plat_mem_thaw(uint64_t addr, size_t size) {
    uint64_t a = addr, end = a + size;
    DWORD old;
    if (VirtualProtect((void *)(uintptr_t)a, size, PAGE_READWRITE, &old)) return 1;
    // Not all committed (the freeze left reserved holes): the read-only runs only. Every committed page of a frozen
    // region was read-write at the freeze, and mapping changes thaw before they run, so these are all ours.
    MEMORY_BASIC_INFORMATION mbi;
    while (a < end && VirtualQuery((void *)(uintptr_t)a, &mbi, sizeof mbi)) {
        uint64_t b = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (b > end) b = end;
        if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READONLY &&
            !VirtualProtect((void *)(uintptr_t)a, b - a, PAGE_READWRITE, &old))
            return 0;
        a = b;
    }
    return 1;
}

void *plat_mem_alloc(size_t n) { return VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }
void plat_mem_free(void *p, size_t n) { (void)n; if (p) VirtualFree(p, 0, MEM_RELEASE); }
void *plat_aligned_alloc(size_t n, size_t align) { return _aligned_malloc(n, align); }
void plat_aligned_free(void *p) { _aligned_free(p); }

// ---- faults and crashes

static int (*g_write_fault)(uint64_t);
static void (*g_crash)(const PlatCrash *);

// The first handler in the chain: gives a write to an unwritable page to plat_on_write_fault's function.
static LONG CALLBACK write_fault_veh(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || er->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    return g_write_fault(er->ExceptionInformation[1]) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}

void plat_on_write_fault(int (*fn)(uint64_t addr)) {
    g_write_fault = fn;
    AddVectoredExceptionHandler(1, write_fault_veh);
}

uint64_t plat_image_base(void) { return (uint64_t)(uintptr_t)GetModuleHandleW(NULL); }

uint64_t plat_image_size(void) {
    MODULEINFO mi;
    return GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(NULL), &mi, sizeof mi) ? mi.SizeOfImage : 0;
}

static LONG CALLBACK crash_veh(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    const char *what = code == EXCEPTION_ACCESS_VIOLATION ? "access violation"
                     : code == EXCEPTION_ILLEGAL_INSTRUCTION ? "illegal instruction"
                     : code == EXCEPTION_INT_DIVIDE_BY_ZERO ? "integer divide by zero"
                     : code == EXCEPTION_PRIV_INSTRUCTION ? "privileged instruction"
                     : code == EXCEPTION_STACK_OVERFLOW ? "stack overflow"
                     : code == EXCEPTION_IN_PAGE_ERROR ? "in-page error" : NULL;
    if (!what) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
    if (rip - (uintptr_t)plat_image_base() >= plat_image_size())
        return EXCEPTION_CONTINUE_SEARCH;   // not our code: leave it to the handler that expects it
    PlatCrash c = {.what = what, .rip = rip, .access = -1};
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        c.access = (int)ep->ExceptionRecord->ExceptionInformation[0];
        c.addr = ep->ExceptionRecord->ExceptionInformation[1];
    }
    g_crash(&c);
    ExitProcess(71);
}

void plat_on_crash(void (*fn)(const PlatCrash *c)) {
    g_crash = fn;
    AddVectoredExceptionHandler(0, crash_veh);
}

void plat_exit_now(int code) { ExitProcess((UINT)code); }

void plat_rerun_self(void) {
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    wchar_t *cmd = _wcsdup(GetCommandLineW());   // CreateProcessW may write its command line
    if (!cmd || !CreateProcessW(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) return;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 70;
    GetExitCodeProcess(pi.hProcess, &code);
    ExitProcess(code);
}

int plat_run_self(char *out, size_t cap, uint32_t *code) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = GetStdHandle(STD_INPUT_HANDLE),
                       .hStdOutput = wr, .hStdError = wr};
    PROCESS_INFORMATION pi;
    wchar_t *cmd = _wcsdup(GetCommandLineW());
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    DWORD e = GetLastError();
    free(cmd);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        SetLastError(e);
        return 0;
    }
    size_t n = 0;
    DWORD got;
    while (n + 1 < cap && ReadFile(rd, out + n, (DWORD)(cap - 1 - n), &got, NULL) && got) n += got;
    out[n] = 0;
    char sink[512];
    while (ReadFile(rd, sink, sizeof sink, &got, NULL) && got) {}
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD c = 1;
    GetExitCodeProcess(pi.hProcess, &c);
    *code = c;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 1;
}

// Both places: the process's environment for child processes, and the C library's copy for getenv.
int plat_setenv(const char *name, const char *value) {
    return SetEnvironmentVariableA(name, value) && !_putenv_s(name, value ? value : "");
}

int plat_full_path(const char *path, char *out, size_t cap) {
    wchar_t w[WPATH], full[WPATH];
    if (!widen(path, w, WPATH)) return 0;
    DWORD n = GetFullPathNameW(w, WPATH, full, NULL);
    return n && n < WPATH && WideCharToMultiByte(CP_UTF8, 0, full, -1, out, (int)cap, NULL, NULL) != 0;
}

long plat_timezone_west(void) {
    long west = 0;
    _get_timezone(&west);
    return west;
}

int plat_temp_dir(char *out, size_t cap) {
    wchar_t w[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, w);
    if (!n || n > MAX_PATH) return 0;
    if (w[n - 1] == L'\\') w[n - 1] = 0;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) != 0;
}
uint32_t plat_last_error(void) { return GetLastError(); }

uint64_t plat_process_age_ms(void) {
    FILETIME created, x0, x1, x2, now;
    GetProcessTimes(GetCurrentProcess(), &created, &x0, &x1, &x2);
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER c = {.LowPart = created.dwLowDateTime, .HighPart = created.dwHighDateTime},
                   n = {.LowPart = now.dwLowDateTime, .HighPart = now.dwHighDateTime};
    return (n.QuadPart - c.QuadPart) / 10000;
}

const char *plat_locale_name(void) {
    static char tag[LOCALE_NAME_MAX_LENGTH];
    wchar_t w[LOCALE_NAME_MAX_LENGTH];
    if (GetUserDefaultLocaleName(w, LOCALE_NAME_MAX_LENGTH)) {
        size_t i = 0;
        for (; w[i] && w[i] < 0x80 && i + 1 < sizeof tag; i++) tag[i] = (char)w[i];
        tag[i] = 0;
    }
    return tag;
}
uint32_t plat_process_id(void) { return GetCurrentProcessId(); }

void plat_thread_signals(void) {}

int plat_random(void *out, size_t n) {
    return !BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

struct PlatSha256 {
    BCRYPT_HASH_HANDLE h;
};

PlatSha256 *plat_sha256_new(void) {
    static BCRYPT_ALG_HANDLE alg;
    BCRYPT_ALG_HANDLE a = __atomic_load_n(&alg, __ATOMIC_ACQUIRE);
    if (!a) {
        if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, NULL, 0)) return NULL;
        BCRYPT_ALG_HANDLE expected = NULL;
        if (!__atomic_compare_exchange_n(&alg, &expected, a, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            BCryptCloseAlgorithmProvider(a, 0);
            a = expected;
        }
    }
    PlatSha256 *s = malloc(sizeof *s);
    if (s && BCryptCreateHash(a, &s->h, NULL, 0, NULL, 0, 0)) { free(s); s = NULL; }
    return s;
}

void plat_sha256_update(PlatSha256 *s, const void *p, uint64_t n) {
    const uint8_t *b = p;
    while (n) {
        ULONG k = n > 0x40000000 ? 0x40000000 : (ULONG)n;
        BCryptHashData(s->h, (PUCHAR)b, k, 0);
        b += k;
        n -= k;
    }
}

void plat_sha256_finish(PlatSha256 *s, uint8_t out[32]) {
    BCryptFinishHash(s->h, out, 32, 0);
    BCryptDestroyHash(s->h);
    free(s);
}

uint64_t plat_rss(void) {
    PROCESS_MEMORY_COUNTERS pmc = {.cb = sizeof pmc};
    return K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? pmc.WorkingSetSize : 0;
}

uint64_t plat_peak_rss(void) {
    PROCESS_MEMORY_COUNTERS pmc = {.cb = sizeof pmc};
    return K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? pmc.PeakWorkingSetSize : 0;
}

int plat_mem_is_rw(uint64_t addr) {
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery((void *)(uintptr_t)addr, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE;
}

int plat_mem_is_readonly(uint64_t addr, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery((void *)(uintptr_t)addr, &mbi, sizeof mbi) && mbi.Protect == PAGE_READONLY && mbi.RegionSize >= n;
}

int plat_mem_read(uint64_t addr, void *out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)addr, out, n, &got) && got == n;
}

int plat_exe_path(char *out, size_t cap) {
    wchar_t w[WPATH];
    DWORD n = GetModuleFileNameW(NULL, w, WPATH);
    if (!n || n >= WPATH) return 0;
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) != 0;
}

size_t plat_getenv(const char *name, char *out, size_t cap) {
    wchar_t wn[256], v[WPATH];
    if (!widen(name, wn, 256)) return 0;
    DWORD n = GetEnvironmentVariableW(wn, v, WPATH);
    if (!n || n >= WPATH) return 0;
    int m = WideCharToMultiByte(CP_UTF8, 0, v, -1, out, (int)cap, NULL, NULL);
    return m ? (size_t)m - 1 : 0;
}

uint32_t plat_thread_id(void) { return GetCurrentThreadId(); }

const char *plat_ui_language(void) {
    static char tag[LOCALE_NAME_MAX_LENGTH];
    static int done;
    if (!done) {
        wchar_t w[LOCALE_NAME_MAX_LENGTH];
        LCID lcid = GetUserDefaultUILanguage();
        int n = LCIDToLocaleName(lcid, w, LOCALE_NAME_MAX_LENGTH, 0);
        if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, tag, sizeof tag, NULL, NULL)) tag[0] = 0;
        done = 1;
    }
    return tag;
}

int plat_font_path(const char *lang, char *out, size_t cap) {
    static const struct { const char *lang; const wchar_t *files[3]; } fonts[] = {
        {"ja", {L"YuGothM.ttc", L"meiryo.ttc", L"msgothic.ttc"}},
        {"ko", {L"malgun.ttf", L"gulim.ttc", NULL}},
        {"zh-Hans", {L"msyh.ttc", L"simsun.ttc", NULL}},
        {"zh-Hant", {L"msjh.ttc", L"mingliu.ttc", NULL}},
    };
    wchar_t dir[MAX_PATH], w[WPATH];
    UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    for (size_t i = 0; i < sizeof fonts / sizeof *fonts; i++) {
        if (strcmp(fonts[i].lang, lang)) continue;
        for (int k = 0; k < 3 && fonts[i].files[k]; k++) {
            swprintf(w, WPATH, L"%ls\\Fonts\\%ls", dir, fonts[i].files[k]);
            DWORD a = GetFileAttributesW(w);
            if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY))
                return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) != 0;
        }
    }
    return 0;
}

int plat_os_version(char *out, size_t cap) {
    // GetVersionEx reports the version the manifest asks for; ntdll's RtlGetVersion the real one
    typedef LONG(WINAPI * RtlGetVersion_t)(OSVERSIONINFOW *);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersion_t get = nt ? (RtlGetVersion_t)(void *)GetProcAddress(nt, "RtlGetVersion") : NULL;
    OSVERSIONINFOW v = {.dwOSVersionInfoSize = sizeof v};
    if (!get || get(&v) != 0) return 0;
    DWORD ubr = 0, size = sizeof ubr;   // the update build revision, the number after the build
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD,
                     NULL, &ubr, &size) != ERROR_SUCCESS)
        ubr = 0;
    snprintf(out, cap, ubr ? "Windows %lu.%lu.%lu.%lu" : "Windows %lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion,
             v.dwBuildNumber, ubr);
    return 1;
}

int plat_stderr_path(char *out, size_t cap) {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    wchar_t w[WPATH];
    if (!h || h == INVALID_HANDLE_VALUE || GetFileType(h) != FILE_TYPE_DISK) return 0;
    DWORD n = GetFinalPathNameByHandleW(h, w, WPATH, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n || n >= WPATH) return 0;
    const wchar_t *p = wcsncmp(w, L"\\\\?\\", 4) ? w : w + 4;   // the \\?\ prefix
    return WideCharToMultiByte(CP_UTF8, 0, p, -1, out, (int)cap, NULL, NULL) != 0;
}

// ---- desktop

int plat_clipboard_set(const char *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0) return 0;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n * sizeof(wchar_t));
    if (!mem) return 0;
    wchar_t *w = GlobalLock(mem);
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    GlobalUnlock(mem);
    // A clipboard opened without a window gets no owner, and SetClipboardData may then fail: a message-only window of
    // the calling thread owns it for the call.
    HWND owner = CreateWindowExW(0, L"STATIC", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
    int ok = 0;
    if (OpenClipboard(owner)) {
        ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, mem) != NULL;
        CloseClipboard();
    }
    if (owner) DestroyWindow(owner);
    if (!ok) GlobalFree(mem);   // after a successful SetClipboardData the system owns it
    return ok;
}

static DWORD WINAPI open_folder_main(void *arg) {
    ShellExecuteW(NULL, L"open", arg, NULL, NULL, SW_SHOWNORMAL);
    free(arg);
    return 0;
}

int plat_open_folder(const char *path) {
    wchar_t *w = malloc(WPATH * sizeof *w);
    if (!w || !widen(path, w, WPATH)) return free(w), 0;
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return free(w), 0;
    // the shell can take a while to answer: not on the caller's thread (the present path)
    HANDLE t = CreateThread(NULL, 0, open_folder_main, w, 0, NULL);
    if (!t) return free(w), 0;
    CloseHandle(t);
    return 1;
}

// ---- dynamic libraries

void *plat_lib_open(const char *name) {
    wchar_t w[WPATH];
    if (!widen(name, w, WPATH)) return NULL;
    int bare = !wcschr(w, L'\\') && !wcschr(w, L'/');
    return (void *)LoadLibraryExW(w, NULL, bare ? LOAD_LIBRARY_SEARCH_SYSTEM32 : 0);
}

void *plat_lib_sym(void *lib, const char *name) { return lib ? (void *)GetProcAddress((HMODULE)lib, name) : NULL; }

// ---- machine load: GetSystemTimes for the CPU, PDH (pdh.dll, loaded on first use) for the thermal zones and the GPU
// engine and adapter memory counters that Task Manager reads.

typedef PDH_STATUS(WINAPI *PdhOpenQueryW_t)(LPCWSTR, DWORD_PTR, PDH_HQUERY *);
typedef PDH_STATUS(WINAPI *PdhAddEnglishCounterW_t)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER *);
typedef PDH_STATUS(WINAPI *PdhCollectQueryData_t)(PDH_HQUERY);
typedef PDH_STATUS(WINAPI *PdhGetFormattedCounterArrayW_t)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD,
                                                           PPDH_FMT_COUNTERVALUE_ITEM_W);
typedef PDH_STATUS(WINAPI *PdhCloseQuery_t)(PDH_HQUERY);
static struct {
    INIT_ONCE once;
    PdhOpenQueryW_t open;
    PdhAddEnglishCounterW_t add;
    PdhCollectQueryData_t collect;
    PdhGetFormattedCounterArrayW_t get;
    PdhCloseQuery_t close;
} pdh = {INIT_ONCE_STATIC_INIT};

static BOOL CALLBACK pdh_load(INIT_ONCE *o, void *p, void **c) {
    (void)o, (void)p, (void)c;
    HMODULE l = plat_lib_open("pdh.dll");
    if (!l) return TRUE;
    pdh.add = (PdhAddEnglishCounterW_t)(void *)GetProcAddress(l, "PdhAddEnglishCounterW");
    pdh.collect = (PdhCollectQueryData_t)(void *)GetProcAddress(l, "PdhCollectQueryData");
    pdh.get = (PdhGetFormattedCounterArrayW_t)(void *)GetProcAddress(l, "PdhGetFormattedCounterArrayW");
    pdh.close = (PdhCloseQuery_t)(void *)GetProcAddress(l, "PdhCloseQuery");
    PdhOpenQueryW_t open = (PdhOpenQueryW_t)(void *)GetProcAddress(l, "PdhOpenQueryW");
    if (pdh.add && pdh.collect && pdh.get && pdh.close) pdh.open = open;   // open set: all of them are there
    return TRUE;
}

// One PDH query with its wildcard counters ("\Object(*)\Counter"). A query whose open failed stays NULL.
typedef struct { PDH_HQUERY q; PDH_HCOUNTER c[2]; } PdhQuery;

static void pdh_query(PdhQuery *pq, const wchar_t *c0, const wchar_t *c1) {
    InitOnceExecuteOnce(&pdh.once, pdh_load, NULL, NULL);
    pq->q = NULL;
    if (!pdh.open || pdh.open(NULL, 0, &pq->q) != ERROR_SUCCESS) {
        pq->q = NULL;
        return;
    }
    if (pdh.add(pq->q, c0, 0, &pq->c[0]) != ERROR_SUCCESS) pq->c[0] = NULL;
    if (!c1 || pdh.add(pq->q, c1, 0, &pq->c[1]) != ERROR_SUCCESS) pq->c[1] = NULL;
    if (!pq->c[0] && !pq->c[1]) {
        pdh.close(pq->q);
        pq->q = NULL;
    }
}

// The counter's instances as a malloc'd array (free it), *count of them; NULL when there is no valid data yet (a rate
// counter needs two collections).
static PDH_FMT_COUNTERVALUE_ITEM_W *pdh_items(PDH_HCOUNTER c, DWORD *count) {
    DWORD size = 0;
    *count = 0;
    if (!c || pdh.get(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, count, NULL) != PDH_MORE_DATA || !size) return NULL;
    PDH_FMT_COUNTERVALUE_ITEM_W *items = malloc(size);
    if (items && pdh.get(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, count, items) == ERROR_SUCCESS) return items;
    free(items);
    *count = 0;
    return NULL;
}

struct PlatLoad {
    uint64_t idle, kernel, user, proc;   // the previous call's times; kernel 0 before the first
    PdhQuery thermal, gpu;               // gpu: engine utilisation, adapter dedicated memory
    wchar_t luid[40];                    // "luid_0x<high>_0x<low>" of the adapter, lower case; empty for any
};

PlatLoad *plat_load_open(const uint8_t *gpu_luid) {
    PlatLoad *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    if (gpu_luid) {   // Vulkan's deviceLUID is the LUID struct's memory: LowPart, then HighPart
        uint32_t lo, hi;
        memcpy(&lo, gpu_luid, 4);
        memcpy(&hi, gpu_luid + 4, 4);
        swprintf(l->luid, sizeof l->luid / sizeof *l->luid, L"luid_0x%08x_0x%08x", hi, lo);
    }
    pdh_query(&l->thermal, L"\\Thermal Zone Information(*)\\Temperature", NULL);
    pdh_query(&l->gpu, L"\\GPU Engine(*)\\Utilization Percentage", L"\\GPU Adapter Memory(*)\\Dedicated Usage");
    return l;
}

void plat_load_close(PlatLoad *l) {
    if (!l) return;
    if (l->thermal.q) pdh.close(l->thermal.q);
    if (l->gpu.q) pdh.close(l->gpu.q);
    free(l);
}

static uint64_t ft64(FILETIME f) { return (uint64_t)f.dwHighDateTime << 32 | f.dwLowDateTime; }

void plat_cpu_load(PlatLoad *l, float *total_pct, float *proc_pct, float *temp_c) {
    *total_pct = *proc_pct = *temp_c = -1;
    if (!l) return;
    FILETIME idle, kern, user, cr, ex, pk, pu;
    if (GetSystemTimes(&idle, &kern, &user) && GetProcessTimes(GetCurrentProcess(), &cr, &ex, &pk, &pu)) {
        uint64_t i = ft64(idle), k = ft64(kern), u = ft64(user), p = ft64(pk) + ft64(pu);
        uint64_t total = (k - l->kernel) + (u - l->user);   // kernel time includes idle time
        if (l->kernel && total) {
            *total_pct = 100.0f * (float)(total - (i - l->idle)) / (float)total;
            *proc_pct = 100.0f * (float)(p - l->proc) / (float)total;
        }
        l->idle = i, l->kernel = k, l->user = u, l->proc = p;
    }
    // the firmware's ACPI zones, near the CPU on most boards; the exact package temperature needs a kernel driver
    DWORD n;
    PDH_FMT_COUNTERVALUE_ITEM_W *it;
    if (l->thermal.q && pdh.collect(l->thermal.q) == ERROR_SUCCESS && (it = pdh_items(l->thermal.c[0], &n))) {
        for (DWORD i = 0; i < n; i++) {   // the hottest zone, in kelvin
            double c = it[i].FmtValue.doubleValue - 273.15;
            if (it[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA && c > *temp_c && c < 150) *temp_c = (float)c;
        }
        free(it);
    }
}

// The adapter part of an instance name, "luid_0x..._0x...", lower case, into out (40 wide characters).
static void inst_luid(const wchar_t *name, wchar_t *out) {
    const wchar_t *p = wcsstr(name, L"luid_");
    size_t n = 0;
    while (p && p[n] && n < 26 && n < 39) out[n] = (wchar_t)towlower(p[n]), n++;   // luid_0x%08x_0x%08x: 26
    out[n] = 0;
}

void plat_gpu_load(PlatLoad *l, float *load_pct, float *vram_used_mb) {
    *load_pct = *vram_used_mb = -1;
    if (!l || !l->gpu.q || pdh.collect(l->gpu.q) != ERROR_SUCCESS) return;
    // Engine instances: "pid_<n>_luid_0x.._0x.._phys_<n>_eng_<n>_engtype_<type>". Per adapter and engine type the
    // processes' shares add up; the load is the busiest engine type.
    enum { TYPES = 32 };
    struct { wchar_t luid[40], type[32]; double sum; } t[TYPES];
    int nt = 0;
    DWORD n;
    PDH_FMT_COUNTERVALUE_ITEM_W *it = pdh_items(l->gpu.c[0], &n);
    for (DWORD i = 0; it && i < n; i++) {
        if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) continue;
        wchar_t lu[40];
        inst_luid(it[i].szName, lu);
        if (l->luid[0] && wcscmp(lu, l->luid)) continue;
        const wchar_t *ty = wcsstr(it[i].szName, L"engtype_");
        ty = ty ? ty + 8 : L"";
        int k = 0;
        while (k < nt && (wcscmp(t[k].luid, lu) || wcsncmp(t[k].type, ty, 31))) k++;
        if (k == nt) {
            if (nt == TYPES) continue;
            wcscpy(t[k].luid, lu);
            wcsncpy(t[k].type, ty, 31);
            t[k].type[31] = 0;
            t[k].sum = 0;
            nt++;
        }
        t[k].sum += it[i].FmtValue.doubleValue;
    }
    free(it);
    for (int k = 0; k < nt; k++)
        if (t[k].sum > *load_pct) *load_pct = (float)(t[k].sum > 100 ? 100 : t[k].sum);
    // Adapter memory instances: "luid_0x.._0x.._phys_<n>", bytes. Without a LUID: the adapter that uses the most.
    it = pdh_items(l->gpu.c[1], &n);
    double sum = 0, best = -1;
    for (DWORD i = 0; it && i < n; i++) {
        if (it[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) continue;
        wchar_t lu[40];
        inst_luid(it[i].szName, lu);
        if (l->luid[0] ? !wcscmp(lu, l->luid) : 0) sum += it[i].FmtValue.doubleValue;
        if (it[i].FmtValue.doubleValue > best) best = it[i].FmtValue.doubleValue;
    }
    if (it && (l->luid[0] ? sum > 0 : best >= 0)) *vram_used_mb = (float)((l->luid[0] ? sum : best) / 1048576.0);
    free(it);
}

void plat_mem_info(float *used_mb, float *total_mb, float *proc_mb) {
    *used_mb = *total_mb = *proc_mb = -1;
    MEMORYSTATUSEX ms = {.dwLength = sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) {
        *total_mb = (float)(ms.ullTotalPhys >> 20);
        *used_mb = (float)((ms.ullTotalPhys - ms.ullAvailPhys) >> 20);
    }
    PROCESS_MEMORY_COUNTERS pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) *proc_mb = (float)(pmc.WorkingSetSize >> 20);
}

void plat_commit_info(double *proc_mb, double *used_mb, double *limit_mb) {
    *proc_mb = *used_mb = *limit_mb = -1;
    MEMORYSTATUSEX ms = {.dwLength = sizeof ms};
    if (GlobalMemoryStatusEx(&ms)) {
        *limit_mb = (double)(ms.ullTotalPageFile >> 20);
        *used_mb = (double)((ms.ullTotalPageFile - ms.ullAvailPageFile) >> 20);
    }
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc))
        *proc_mb = (double)(pmc.PrivateUsage >> 20);
}

// ---- child processes

struct PlatChild {
    HANDLE proc, in, out;                                  // in: our write end, out: our read end
};

PlatChild *plat_child_start(const char *exe, const char *arg, uint32_t *err) {
    wchar_t we[WPATH], wa[256] = L"", cmd[WPATH + 300];
    if (err) *err = 0;
    if (!widen(exe, we, WPATH) || (arg && !widen(arg, wa, 256))) return NULL;
    swprintf(cmd, WPATH + 300, arg ? L"\"%ls\" %ls" : L"\"%ls\"", we, wa);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE in_r, in_w, out_r, out_w;
    // Pipes are created and handed out one child at a time: a child started meanwhile would inherit the other pipe
    // ends and keep them open after their owner closed them.
    static SRWLOCK spawn = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&spawn);
    if (!CreatePipe(&in_r, &in_w, &sa, 0)) return ReleaseSRWLockExclusive(&spawn), NULL;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
        CloseHandle(in_r), CloseHandle(in_w);
        return ReleaseSRWLockExclusive(&spawn), NULL;
    }
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    STARTUPINFOW si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = in_r, .hStdOutput = out_w,
                       .hStdError = nul != INVALID_HANDLE_VALUE ? nul : out_w};
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    DWORD e = GetLastError();
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    ReleaseSRWLockExclusive(&spawn);
    if (!ok) {
        CloseHandle(in_w), CloseHandle(out_r);
        if (err) *err = e;
        return NULL;
    }
    CloseHandle(pi.hThread);
    PlatChild *c = malloc(sizeof *c);
    if (!c) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess), CloseHandle(in_w), CloseHandle(out_r);
        return NULL;
    }
    c->proc = pi.hProcess, c->in = in_w, c->out = out_r;
    return c;
}

void plat_child_stop(PlatChild *c) {
    if (!c) return;
    CloseHandle(c->in);
    CloseHandle(c->out);
    TerminateProcess(c->proc, 1);
    CloseHandle(c->proc);
    free(c);
}

int plat_child_write(PlatChild *c, const void *data, size_t n) {
    DWORD put = 0;
    return WriteFile(c->in, data, (DWORD)n, &put, NULL) && put == n;
}

long plat_child_read(PlatChild *c, void *buf, size_t cap) {
    DWORD got = 0;
    if (!ReadFile(c->out, buf, (DWORD)cap, &got, NULL)) return -1;
    return (long)got;
}
