// Platform layer: threads, time, locks. New runtime code calls this instead of the OS;
// one implementation per OS (win32.c, posix.c). The FP control register lives in the per-architecture fp_*.h headers,
// inline, because lifted FP code touches it on every operation.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if defined(__x86_64__) || defined(_M_X64)
#include "fp_x86.h"
#endif

#ifdef _WIN32
#define PLAT_MAX_PATH 260
#define PLAT_SEP "\\"
#define PLAT_EXE_SUFFIX ".exe"
#define strtok_r strtok_s
#define tzset _tzset
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#define PLAT_MAX_PATH 4096
#define PLAT_SEP "/"
#define PLAT_EXE_SUFFIX ""
#endif

// ---- threads
typedef struct PlatThread PlatThread;
typedef void (*PlatThreadFn)(void *arg);

// Starts fn(arg) on a new thread named name (shown in debuggers and profilers; may be NULL). NULL on failure.
PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name);
// The same with a stack of stack_bytes, reserved and backed on demand; 0: the default.
PlatThread *plat_thread_new_stack(PlatThreadFn fn, void *arg, const char *name, size_t stack_bytes);
void plat_thread_join(PlatThread *t);     // waits for fn to return, then frees t
void plat_thread_detach(PlatThread *t);   // frees t; the thread runs on
// Restricts t (NULL: the calling thread) to the logical processors in mask (bit n = processor n of the first group).
// Returns the previous mask, 0 on failure.
uint64_t plat_thread_affinity(PlatThread *t, uint64_t mask);
uint32_t plat_cpu_count(void);            // logical processors
void plat_thread_name(const char *name);  // names the calling thread for debuggers and profilers
// Scheduling class of t (NULL: the calling thread): PLAT_PRIO_*, or -2 to 2 around normal. A hint; a system that does not
// allow the change leaves the thread as it is.
void plat_thread_priority(PlatThread *t, int level);
int plat_thread_priority_get(void);       // the calling thread's level
enum { PLAT_PRIO_IDLE = -15, PLAT_PRIO_BELOW = -1, PLAT_PRIO_NORMAL = 0, PLAT_PRIO_HIGHEST = 2 };
int plat_thread_wait_ms(PlatThread *t, uint32_t ms);   // 1 once t has finished, 0 on timeout; plat_thread_join still follows
void plat_yield(void);                    // offers the rest of the time slice to another ready thread

// ---- time
uint64_t plat_now_ns(void);               // monotonic, high resolution, arbitrary epoch
// t * num / den for a fixed num / den without a 128-bit division per call (__udivti3 took 5% of the game's running
// samples through plat_now_ns and rt_ticks): t * q + (t * f >> 64) with q = num / den and f = (num % den) * 2^64 / den.
// Monotonic in t and at most one below the exact quotient while t * (num % den) / den fits 64 bits.
typedef struct { uint64_t q, f; } PlatScale;
static inline PlatScale plat_scale(uint64_t num, uint64_t den) {
    return (PlatScale){num / den, (uint64_t)(((unsigned __int128)(num % den) << 64) / den)};
}
static inline uint64_t plat_scaled(PlatScale s, uint64_t t) {
    return t * s.q + (uint64_t)(((unsigned __int128)t * s.f) >> 64);
}
void plat_sleep_ns(uint64_t ns);          // returns no earlier than ns later; overshoot well under 1 ms
// Waits about ns on the system's best timer without the spinning plat_sleep_ns ends with: for loops that pace themselves
// against plat_now_ns.
void plat_wait_ns(uint64_t ns);
void plat_sleep_ms(uint32_t ms);          // a plain sleep, at the system's own granularity; costs no CPU
uint64_t plat_tick_ms(void);              // monotonic milliseconds, cheap and coarse, arbitrary epoch
uint64_t plat_counter(void);              // the raw high-resolution counter, cheapest clock there is
uint64_t plat_counter_hz(void);           // its ticks per second; constant for the process
void plat_timer_fine(void);               // asks for 1 ms wait granularity where the system's default is coarser

// CPU time the thread with this plat_thread_id has used, nanoseconds; 0 when unknown.
uint64_t plat_thread_cpu_ns(uint32_t tid);

// Interrupting a running thread. An interrupt runs fn with the thread's instruction pointer; a nonzero return is where
// the thread goes on instead, all its registers as they were. fn must not take locks: the thread may hold any.
//   win32.c runs fn on the caller, with the target suspended, and gives the result back: PLAT_INT_REDIRECTED,
//   PLAT_INT_LEFT or PLAT_INT_FAILED.
//   posix.c sends a signal and returns PLAT_INT_SENT at once; fn runs later in the signal handler, on the target's own
//   thread (arg is then NULL), and the caller does not learn what it decided.
// The target must have called plat_thread_signals, and be alive while the caller holds its reference.
typedef struct PlatThreadRef PlatThreadRef;
typedef uint64_t (*PlatInterruptFn)(uint64_t rip, void *arg);
enum { PLAT_INT_FAILED = -1, PLAT_INT_LEFT = 0, PLAT_INT_REDIRECTED = 1, PLAT_INT_SENT = 2 };
void plat_interrupt_init(PlatInterruptFn fn);
PlatThreadRef *plat_thread_ref_self(void);                // NULL on failure
void plat_thread_ref_close(PlatThreadRef *r);
int plat_thread_interrupt(PlatThreadRef *r, void *arg);

// Fibers: stacks one thread switches between by hand. A thread converts itself before its first switch. fn must not
// return; it switches away for good. A fiber may later run on another thread. Each keeps its own FP control state.
typedef struct PlatFiber PlatFiber;
PlatFiber *plat_fiber_convert(void);                      // the calling thread as a fiber; NULL on failure
void plat_fiber_unconvert(void);                          // back to a plain thread, on its own stack
PlatFiber *plat_fiber_new(size_t stack_bytes, void (*fn)(void *arg), void *arg);   // reserved, backed on demand
void plat_fiber_switch(PlatFiber *to);
void plat_fiber_delete(PlatFiber *f);                     // not the one running

// ---- locks (not recursive) and condition variables; zero-initialised storage is a valid unlocked lock and an empty
// condition variable (PLAT_MUTEX_INIT / PLAT_COND_INIT spell it). A lock can be taken shared or exclusive, and the same
// lock waits on a condition variable (held exclusive); pthreads has no one type that does both, which is why posix.c
// builds them on futexes. Waits may wake spuriously.
typedef struct { void *p; } PlatMutex;
typedef struct { void *p; } PlatCond;
#define PLAT_MUTEX_INIT {0}
#define PLAT_COND_INIT {0}
void plat_mutex_init(PlatMutex *m);
void plat_mutex_lock(PlatMutex *m);
int plat_mutex_trylock(PlatMutex *m);     // 1 when acquired
void plat_mutex_unlock(PlatMutex *m);
void plat_mutex_lock_shared(PlatMutex *m);
int plat_mutex_trylock_shared(PlatMutex *m);   // 1 when acquired
void plat_mutex_unlock_shared(PlatMutex *m);
void plat_cond_init(PlatCond *c);
void plat_cond_wait(PlatCond *c, PlatMutex *m);
int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns);   // 0 on timeout (spurious wakeups return 1 too)
void plat_cond_signal(PlatCond *c);
void plat_cond_broadcast(PlatCond *c);

// A recursive lock: the owner may take it again, and releases it as many times. Zero-initialised storage is unlocked.
typedef struct { PlatMutex m; uint32_t owner; int depth; } PlatRecMutex;
void plat_rmutex_lock(PlatRecMutex *r);
int plat_rmutex_trylock(PlatRecMutex *r);                 // 1 when acquired
void plat_rmutex_unlock(PlatRecMutex *r);

// One-time initialisation: runs fn once however many threads arrive; they all return after it finished.
typedef struct { void *p; } PlatOnce;
#define PLAT_ONCE_INIT {0}
void plat_once(PlatOnce *o, void (*fn)(void));

// Events: a flag threads wait for. A manual event stays set until reset; an automatic one lets one waiter through and
// clears itself.
#define PLAT_WAIT_FOREVER 0xffffffffu
typedef struct PlatEvent PlatEvent;
PlatEvent *plat_event_new(int manual, int initially_set);   // NULL on failure
void plat_event_free(PlatEvent *e);
void plat_event_set(PlatEvent *e);
void plat_event_reset(PlatEvent *e);
void plat_event_wait(PlatEvent *e);
int plat_event_wait_ms(PlatEvent *e, uint32_t ms);          // 1 when set, 0 on timeout; ms 0 only polls, PLAT_WAIT_FOREVER waits

// ---- memory
// Fixed addresses. Reserved pages have no access and take no RAM until committed. With watch set, writes to committed
// pages of the range are recorded for plat_mem_written; the range must be released whole.
int plat_mem_reserve(uint64_t addr, size_t n, int watch); // 1 on success; 0 when any of it is taken
void plat_mem_release(uint64_t addr, size_t n);
int plat_mem_commit(uint64_t addr, size_t n);             // read-write pages inside a reserved range; 1 on success
int plat_mem_decommit(uint64_t addr, size_t n);           // back to reserved; the next commit reads zeros. 1 on success
enum { PLAT_PROT_NONE, PLAT_PROT_READ, PLAT_PROT_RW };
int plat_mem_protect(uint64_t addr, size_t n, int prot);  // committed pages; 1 on success
// The pages (4 KiB addresses, ascending) written in [addr, addr + n) of a watched reservation since the last reset, at
// most max; with reset those reported start clean again and the rest stay recorded. UINT64_MAX on failure.
uint64_t plat_mem_written(uint64_t addr, size_t n, uint64_t *pages, uint64_t max, int reset);
// Read-only "freezing" of a watched range, for systems where asking for written pages costs time proportional to the
// range. Freeze returns 1 when the committed pages of [addr, addr + n), all read-write, became read-only, 0 when it
// declined: posix.c always declines, as PAGEMAP_SCAN is already fast. Thaw makes the read-only ones read-write again.
int plat_mem_can_freeze(void);
int plat_mem_freeze(uint64_t addr, size_t n);
int plat_mem_thaw(uint64_t addr, size_t n);               // 1 on success
void *plat_mem_alloc(size_t n);                           // zeroed pages anywhere, backed on first touch; NULL on failure
void plat_mem_free(void *p, size_t n);
void *plat_aligned_alloc(size_t n, size_t align);         // free with plat_aligned_free
void plat_aligned_free(void *p);
int plat_mem_is_rw(uint64_t addr);                        // 1 for a committed read-write page; posix.c: always 0
int plat_mem_is_readonly(uint64_t addr, size_t n);        // 1 when all of [addr, addr + n) is one read-only mapping
int plat_mem_read(uint64_t addr, void *out, size_t n);    // copies n bytes, 0 instead of faulting on unmapped memory

// UTF-8 to UTF-16, NUL included, at most cap units; the count written, 0 when it does not fit or is not valid UTF-8.
size_t plat_utf8_to_utf16(const char *s, uint16_t *out, size_t cap);

// ---- random bytes and SHA-256
int plat_random(void *out, size_t n);                     // from the system's generator; 1 on success
typedef struct PlatSha256 PlatSha256;
PlatSha256 *plat_sha256_new(void);                        // NULL when unavailable
void plat_sha256_update(PlatSha256 *s, const void *p, uint64_t n);
void plat_sha256_finish(PlatSha256 *s, uint8_t out[32]);  // frees s

// ---- faults and crashes
// fn runs first on a write to a mapped but unwritable page; a return of 1 retries the write. At most one.
void plat_on_write_fault(int (*fn)(uint64_t addr));
typedef struct {
    const char *what;      // "access violation", "illegal instruction", ...
    uint64_t rip;          // the faulting host instruction
    uint64_t addr;         // the address an access violation touched, 0 otherwise
    int access;            // 0 read, 1 write, 8 execute, -1 not an access violation
} PlatCrash;
// fn runs on a fatal fault in this executable's own code (faults in other libraries go to their handlers); it ends the
// process. Survives a stack overflow only where the system gives it another stack (posix.c: plat_thread_signals).
void plat_on_crash(void (*fn)(const PlatCrash *c));
// Sets up what signals and faults need on the calling thread (posix.c: an alternate signal stack, so a stack overflow
// or a preemption signal never writes to the thread's own stack). Every thread that runs guest code calls it first.
void plat_thread_signals(void);
void plat_exit_now(int code);                             // ends the process at once: no atexit handlers, no flush
uint64_t plat_image_base(void);                           // where this executable is loaded
uint64_t plat_image_size(void);
// Re-runs this process with the same command line, waits for it and exits with its code; returns only on failure.
// Environment changes made with plat_setenv reach the new process.
void plat_rerun_self(void);
// Runs this executable again with the same command line and the current environment, waits for it, and puts its stdout
// and stderr, NUL-terminated and cut to cap, into out. *code gets its exit code. 0 when it could not start.
int plat_run_self(char *out, size_t cap, uint32_t *code);
int plat_setenv(const char *name, const char *value);     // value NULL removes it; 1 on success
int plat_full_path(const char *path, char *out, size_t cap);   // absolute form of a path that may not exist; 1 on success
long plat_timezone_west(void);                            // seconds west of UTC of the local zone, without daylight saving
int plat_temp_dir(char *out, size_t cap);                 // the directory for temporary files, no trailing separator
uint64_t plat_process_age_ms(void);                       // milliseconds since this process started
const char *plat_locale_name(void);                       // the user's locale as a BCP-47 tag; "" when unknown
uint32_t plat_last_error(void);                           // the calling thread's last OS error, for log lines
uint32_t plat_process_id(void);
uint64_t plat_rss(void);                                  // bytes; the process's working set now
uint64_t plat_peak_rss(void);                             // bytes; the process's largest working set so far

// ---- files. Paths are UTF-8 with either separator; each implementation converts to what the OS wants.
int plat_file_exists(const char *path);                   // 1 for a file (not a directory)
void plat_file_mkdir_p(const char *path);                 // creates every missing directory of path, itself included
// Whole file into a malloc'd buffer with a NUL after the last byte; NULL when missing, empty or over 1 GiB.
uint8_t *plat_file_read(const char *path, uint64_t *size);
int plat_file_write(const char *path, const void *data, size_t n);   // create or truncate; 1 when all n bytes landed
int plat_file_replace(const char *from, const char *to);  // rename, replacing to (write-through); 1 on success
// Writes path.<pid>.<tid>.tmp, flushes it to the disk and renames it over path: a crash leaves the old file or the new
// one, never a torn or zero-filled one. A read-only path stays as it is and fails. 1 on success.
int plat_file_write_atomic(const char *path, const void *data, size_t n);
int plat_file_flush(const char *path);                   // flushes a written file's data to the disk; 1 on success
int plat_file_delete(const char *path);                   // 1 on success
int plat_file_truncate(const char *path, uint64_t size);  // 1 on success
int64_t plat_file_size(const char *path);                 // bytes of a file, -1 when missing or unreadable
int plat_file_set_readonly(const char *path, int on);     // sets or clears the read-only attribute; 1 on success
// A file opened for positioned I/O: reads and writes at explicit offsets, safe from several threads at once. They
// transfer until done or end of file and return the bytes moved; -1 on an error, even after a partial transfer, so a
// failed read is never mistaken for a short file.
typedef struct PlatFile PlatFile;
enum { PLAT_FILE_READ = 1, PLAT_FILE_WRITE = 2, PLAT_FILE_CREATE = 4, PLAT_FILE_TRUNC = 8,
       PLAT_FILE_UNBUFFERED = 16,     // past the system's file cache; offsets and lengths page-aligned
       PLAT_FILE_EXCL = 32,           // with CREATE: fails when the file exists
       PLAT_FILE_PARALLEL = 64 };     // reads from several threads run side by side, where a system serialises them
PlatFile *plat_file_open(const char *path, int flags);    // NULL on failure, with plat_last_error set
void plat_file_close(PlatFile *f);
int64_t plat_file_pread(PlatFile *f, void *buf, uint64_t n, uint64_t at);
int64_t plat_file_pwrite(PlatFile *f, const void *buf, uint64_t n, uint64_t at);
int64_t plat_file_handle_size(PlatFile *f);               // -1 on failure
int plat_file_set_size(PlatFile *f, uint64_t n);          // 1 on success
int plat_file_sync(PlatFile *f);                          // to the disk; 1 on success
// What tells one file's version from another: all equal means the same file, unchanged. Stored in files, so stable.
typedef struct { uint32_t volume, pad; uint64_t file_id, size, write_time, create_time; } PlatFileId;
int plat_file_identity(PlatFile *f, PlatFileId *id);      // 1 on success
const void *plat_file_map(PlatFile *f);                   // the whole file mapped read-only, for the process's life; NULL on failure
int plat_file_links(const char *path);                    // hard links of a file; 0 when absent
int plat_dir_exists(const char *path);                    // 1 for a directory
int plat_dir_remove(const char *path);                    // an empty directory; 1 on success
int plat_file_move(const char *from, const char *to);     // a file or directory, failing if to exists; 1 on success
// The same, on the disk when it returns: a crash after it finds the new name, a crash during it one of the two.
int plat_file_move_durable(const char *from, const char *to);
int plat_file_link(const char *existing, const char *link);   // a hard link; 1 on success
int plat_dir_sync(const char *path);                      // a directory's entries to the disk, as far as the system can
// What is at a path, without following a final link: 0 when nothing is or it cannot be seen (plat_last_error says which).
enum { PLAT_PATH_EXISTS = 1, PLAT_PATH_DIR = 2, PLAT_PATH_LINK = 4, PLAT_PATH_READONLY = 8 };
int plat_path_info(const char *path);
// plat_last_error values by what they mean to a caller.
enum { PLAT_ERR_OTHER, PLAT_ERR_NOT_FOUND, PLAT_ERR_EXISTS, PLAT_ERR_NOT_EMPTY, PLAT_ERR_BUSY, PLAT_ERR_BAD_BUFFER };
int plat_error_kind(uint32_t error);
int plat_dir_make(const char *path);                      // one directory; 1 also when it already exists
int plat_dir_create(const char *path);                    // one directory; fails when it already exists; 1 on success
// Entries of a directory, without . and ..; order unspecified. The name is valid until the next call.
typedef struct { const char *name; int is_dir, is_link; uint64_t size; } PlatDirEntry;
typedef struct PlatDir PlatDir;
PlatDir *plat_dir_open(const char *dir);                  // NULL when it cannot be listed, with plat_last_error set
int plat_dir_next(PlatDir *d, PlatDirEntry *e);           // 0 at the end
void plat_dir_close(PlatDir *d);
FILE *plat_fopen(const char *path, const char *mode);     // fopen with a UTF-8 path
int plat_file_sync_stream(FILE *f);                       // a flushed stream's file to the disk; 1 on success
int plat_fseek(FILE *f, int64_t at, int whence);
int64_t plat_ftell(FILE *f);
// A file that only grows and other processes may read meanwhile: each write lands whole at its end, from any thread,
// and reaches the file even if the process dies right after (no user-space buffer, no flush to the disk).
typedef struct PlatAppend PlatAppend;
PlatAppend *plat_append_open(const char *path);           // creates or truncates path; NULL on failure
int plat_append_write(PlatAppend *f, const void *data, size_t n);   // 1 when all n bytes landed
// Calls cb(name, ctx) for each file in dir whose name ends in ext (".in"); name has no directory part. cb returns 0 to
// stop. Returns 0 when dir cannot be listed.
int plat_file_list(const char *dir, const char *ext, int (*cb)(const char *name, void *ctx), void *ctx);
int plat_exe_path(char *out, size_t cap);                 // path of the running executable; 0 on failure
// Environment variable as UTF-8 into out; its length, or 0 when unset, empty or too long for cap.
size_t plat_getenv(const char *name, char *out, size_t cap);
// The user's UI language as a BCP-47 tag ("ru-RU", "en-US"); "" when the system gives none. Valid for the process.
const char *plat_ui_language(void);
uint32_t plat_thread_id(void);                            // id of the calling thread, for unique file names
// A system font file with the glyphs of a catalog language ("ja", "ko", "zh-Hans", "zh-Hant"): the menu shows each
// language's name in its own script. Copies the UTF-8 path into out; 0 when the system has none.
int plat_font_path(const char *lang, char *out, size_t cap);
// The operating system and its build ("Windows 10.0.28000") into out; 0 when unknown.
int plat_os_version(char *out, size_t cap);
// The file the process's stderr (the log) goes to, as a UTF-8 path; 0 when stderr is not a file (a console, a pipe).
int plat_stderr_path(char *out, size_t cap);

// ---- desktop: the in-game menu's "Copy system info" and "Open log folder"
int plat_clipboard_set(const char *utf8);                 // replaces the clipboard's text; 1 on success
int plat_open_folder(const char *path);                   // shows the folder in the file manager; 1 on success

// ---- dynamic libraries: a library some machines lack (a GPU vendor's), loaded at run time. A bare name is searched
// in the system directories only. NULL when missing.
void *plat_lib_open(const char *name);
void *plat_lib_sym(void *lib, const char *name);

// ---- machine load for the performance overlay (perfmon.c's sampling thread; each call can take milliseconds). A number
// the system does not give is -1.
typedef struct PlatLoad PlatLoad;
// Opens the counters: CPU times, the thermal zones, and the GPU engine and memory counters of the adapter whose LUID is
// gpu_luid (8 bytes, Vulkan's deviceLUID; NULL: the busiest adapter). Never NULL.
PlatLoad *plat_load_open(const uint8_t *gpu_luid);
void plat_load_close(PlatLoad *l);
// Since the previous call on l (the first call gives -1): CPU use of the machine and of this process, percent of all
// logical processors, and the hottest ACPI thermal zone in degrees Celsius.
void plat_cpu_load(PlatLoad *l, float *total_pct, float *proc_pct, float *temp_c);
// Since the previous call on l: the busiest GPU engine type in percent (as Task Manager shows it), and the adapter's
// dedicated memory in use in MB. Any GPU vendor.
void plat_gpu_load(PlatLoad *l, float *load_pct, float *vram_used_mb);
// Physical memory in MB: in use and installed on the machine, and this process's working set.
void plat_mem_info(float *used_mb, float *total_mb, float *proc_mb);
// Commit charge in MB: this process's private bytes, the system's commit in use and its limit (RAM plus page files).
// An allocation fails when the system's reaches the limit, however much physical memory is free. -1 when unknown.
void plat_commit_info(double *proc_mb, double *used_mb, double *limit_mb);

// ---- child processes: a program with stdin and stdout piped to us (stderr discarded), no console window.
typedef struct PlatChild PlatChild;
// Starts "exe" followed by arg (arg may be NULL). NULL on failure; *err (may be NULL) gets the OS error code.
// Children start one at a time so none inherits another pipe ends.
PlatChild *plat_child_start(const char *exe, const char *arg, uint32_t *err);
void plat_child_stop(PlatChild *c);                       // kills the child and frees c
int plat_child_write(PlatChild *c, const void *data, size_t n);   // 1 when all n bytes were written
long plat_child_read(PlatChild *c, void *buf, size_t cap);        // bytes read (at least 1); 0 or negative: pipe broke
