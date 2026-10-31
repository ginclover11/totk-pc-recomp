// --probe-platform: runtime/platform/ on its own, with no game files and no window. Locks shared and exclusive under
// contention, condition variables, events, once, recursive locks, fibers, write tracking, files and SHA-256.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"

static int fails;
#define CK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

// ---- locks
#define THREADS 8
#define ROUNDS 200000
static PlatMutex m = PLAT_MUTEX_INIT;
static uint64_t a, b;
static volatile int readers_bad;

static void excl_main(void *p) {
    (void)p;
    for (int i = 0; i < ROUNDS; i++) {
        plat_mutex_lock(&m);
        a++;
        b = a;
        plat_mutex_unlock(&m);
    }
}

static void rw_main(void *arg) {
    int writer = (int)(intptr_t)arg % 4 == 0;
    for (int i = 0; i < ROUNDS / 4; i++) {
        if (writer) {
            plat_mutex_lock(&m);
            a++;
            b = a;
            plat_mutex_unlock(&m);
        } else {
            plat_mutex_lock_shared(&m);
            if (a != b) __atomic_add_fetch(&readers_bad, 1, __ATOMIC_RELAXED);
            plat_mutex_unlock_shared(&m);
        }
    }
}

// A bounded queue on one lock and two condition variables, some of the waits timed.
#define QUEUE 8
#define ITEMS 100000
static PlatMutex qm = PLAT_MUTEX_INIT;
static PlatCond not_empty = PLAT_COND_INIT, not_full = PLAT_COND_INIT;
static int queued, produced, consumed;

static void producer(void *p) {
    (void)p;
    for (;;) {
        plat_mutex_lock(&qm);
        while (queued == QUEUE && produced < ITEMS) plat_cond_wait_ns(&not_full, &qm, 200000);
        if (produced >= ITEMS) {
            plat_cond_broadcast(&not_empty);
            plat_mutex_unlock(&qm);
            return;
        }
        queued++, produced++;
        plat_cond_signal(&not_empty);
        plat_mutex_unlock(&qm);
    }
}

static void consumer(void *p) {
    (void)p;
    for (;;) {
        plat_mutex_lock(&qm);
        while (!queued && consumed < ITEMS) plat_cond_wait(&not_empty, &qm);
        if (consumed >= ITEMS) {
            plat_cond_broadcast(&not_empty);
            plat_mutex_unlock(&qm);
            return;
        }
        queued--, consumed++;
        if (consumed == ITEMS) plat_cond_broadcast(&not_empty);
        plat_cond_signal(&not_full);
        plat_mutex_unlock(&qm);
    }
}

static void probe_locks(void) {
    PlatThread *t[16];
    for (int i = 0; i < THREADS; i++) t[i] = plat_thread_new(excl_main, NULL, "probe");
    for (int i = 0; i < THREADS; i++) plat_thread_join(t[i]);
    CK(a == (uint64_t)THREADS * ROUNDS, "exclusive lock lost updates: %llu", (unsigned long long)a);
    a = b = 0;
    for (int i = 0; i < 12; i++) t[i] = plat_thread_new(rw_main, (void *)(intptr_t)i, "probe");
    for (int i = 0; i < 12; i++) plat_thread_join(t[i]);
    CK(!readers_bad, "a reader ran beside a writer %d times", readers_bad);
    CK(a == (uint64_t)3 * (ROUNDS / 4), "writer updates lost: %llu", (unsigned long long)a);
    for (int i = 0; i < 4; i++) t[i] = plat_thread_new(producer, NULL, "probe");
    for (int i = 4; i < 8; i++) t[i] = plat_thread_new(consumer, NULL, "probe");
    for (int i = 0; i < 8; i++) plat_thread_join(t[i]);
    CK(produced == ITEMS && consumed == ITEMS && !queued, "queue ended %d %d %d", produced, consumed, queued);
    plat_mutex_lock(&qm);
    uint64_t t0 = plat_now_ns();
    int r = plat_cond_wait_ns(&not_empty, &qm, 20000000);
    plat_mutex_unlock(&qm);
    CK(!r && plat_now_ns() - t0 >= 15000000, "timed wait returned %d after %llu ns", r, (unsigned long long)(plat_now_ns() - t0));
    static PlatRecMutex rec;
    plat_rmutex_lock(&rec);
    CK(plat_rmutex_trylock(&rec), "a recursive lock refused its owner");
    plat_rmutex_unlock(&rec);
    plat_rmutex_unlock(&rec);
    printf("locks: exclusive, shared, condition variables, recursive: %s\n", fails ? "FAIL" : "ok");
}

// ---- events and once
static PlatEvent *ev_auto;
static volatile int through;
static void ev_main(void *p) {
    (void)p;
    plat_event_wait(ev_auto);
    __atomic_add_fetch(&through, 1, __ATOMIC_SEQ_CST);
}

static PlatOnce once = PLAT_ONCE_INIT;
static volatile int once_runs;
static void once_fn(void) { plat_sleep_ms(5); once_runs++; }
static void once_main(void *p) {
    (void)p;
    plat_once(&once, once_fn);
    CK(once_runs == 1, "once returned before it finished");
}

static void probe_events(void) {
    int before = fails;
    PlatThread *t[16];
    ev_auto = plat_event_new(0, 0);
    for (int i = 0; i < 4; i++) t[i] = plat_thread_new(ev_main, NULL, "probe");
    plat_sleep_ms(10);
    CK(!through, "an automatic event let a waiter through unset");
    plat_event_set(ev_auto);
    plat_sleep_ms(30);
    CK(through == 1, "an automatic event set once let %d through", through);
    for (int i = 1; i < 4; i++) {   // one by one: setting a set event adds nothing, as on Windows
        plat_event_set(ev_auto);
        plat_sleep_ms(30);
    }
    for (int i = 0; i < 4; i++) plat_thread_join(t[i]);
    plat_event_free(ev_auto);
    PlatEvent *man = plat_event_new(1, 0);
    CK(!plat_event_wait_ms(man, 5), "a manual event unset");
    plat_event_set(man);
    CK(plat_event_wait_ms(man, 0) && plat_event_wait_ms(man, PLAT_WAIT_FOREVER), "a manual event stays set");
    plat_event_reset(man);
    CK(!plat_event_wait_ms(man, 0), "a manual event reset");
    plat_event_free(man);
    for (int i = 0; i < 16; i++) t[i] = plat_thread_new(once_main, NULL, "probe");
    for (int i = 0; i < 16; i++) plat_thread_join(t[i]);
    CK(once_runs == 1, "once ran %d times", once_runs);
    printf("events and once: %s\n", fails > before ? "FAIL" : "ok");
}

// ---- fibers: each keeps its stack and FP control word
static PlatFiber *f_main, *f_a, *f_b;
static volatile int f_steps;
static uint32_t f_mxcsr_a, f_mxcsr_b;

static void fiber_b(void *arg) {
    volatile uint64_t on_stack = (uint64_t)(uintptr_t)arg;
    plat_fp_set(plat_fp_get() | PLAT_FP_RC_MASK);
    f_mxcsr_b = plat_fp_get();
    for (;;) {
        f_steps++;
        plat_fiber_switch(f_a);
        CK(on_stack == (uint64_t)(uintptr_t)arg, "fiber b's stack changed");
        CK(plat_fp_get() == f_mxcsr_b, "fiber b's FP control word changed");
    }
}

static void fiber_a(void *arg) {
    volatile uint64_t on_stack = (uint64_t)(uintptr_t)arg;
    f_mxcsr_a = plat_fp_get();
    for (;;) {
        f_steps++;
        plat_fiber_switch(f_b);
        CK(on_stack == (uint64_t)(uintptr_t)arg, "fiber a's stack changed");
        CK(plat_fp_get() == f_mxcsr_a, "fiber a's FP control word changed");
        if (f_steps >= 20000) plat_fiber_switch(f_main);
    }
}

static void probe_fibers(void) {
    int before = fails;
    uint32_t mine = plat_fp_get();
    f_main = plat_fiber_convert();
    CK(f_main != NULL, "converting the thread");
    f_a = plat_fiber_new(1 << 20, fiber_a, (void *)(uintptr_t)0x1111);
    f_b = plat_fiber_new(1 << 20, fiber_b, (void *)(uintptr_t)0x2222);
    CK(f_a && f_b, "creating fibers");
    plat_fiber_switch(f_a);
    CK(f_steps >= 20000, "fibers took %d steps", f_steps);
    CK(plat_fp_get() == mine, "the thread's FP control word changed");
    plat_fiber_delete(f_a);
    plat_fiber_delete(f_b);
    plat_fiber_unconvert();
    printf("fibers: %d switches: %s\n", f_steps, fails > before ? "FAIL" : "ok");
}

// ---- write tracking: a large reservation with a committed part, as memory.c uses it
static void probe_memory(void) {
    int before = fails;
    uint64_t base = 0x7040000000ull, heap = base + 0x30000000, pages[8], n;
    CK(plat_mem_reserve(base, 1ull << 30, 1), "reserving a watched range");
    CK(!plat_mem_reserve(base, 0x1000, 0), "a reservation over a reservation");
    uint8_t byte;
    CK(!plat_mem_read(heap, &byte, 1), "a reserved page is readable");
    CK(plat_mem_commit(heap, 0x200000), "commit");
    plat_mem_written(heap, 0x200000, pages, 8, 1);   // whatever the commit itself marked
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)heap;
    CK(p[0x5000] == 0, "a fresh page is zero");
    p[0x5000] = 1;
    n = plat_mem_written(heap, 0x200000, pages, 8, 1);
    CK(n == 1 && pages[0] == heap + 0x5000, "one write: %llu reported", (unsigned long long)n);
    CK(!plat_mem_written(heap, 0x200000, pages, 8, 1), "a second scan is not clean");
    p[0x7000] = 2, p[0x9000] = 3, p[0xA000] = 4;
    CK(plat_mem_written(heap, 0x200000, pages, 8, 0) == 3, "a peek");
    n = plat_mem_written(heap, 0x200000, pages, 2, 1);
    uint64_t m2 = plat_mem_written(heap, 0x200000, pages + 2, 6, 1);
    CK(n == 2 && m2 == 1 && pages[2] == heap + 0xA000, "max 2: %llu then %llu", (unsigned long long)n, (unsigned long long)m2);
    p[0x3000] = 5;
    n = plat_mem_written(base, 1ull << 30, pages, 8, 1);   // across reserved holes, which were not written
    CK(n == 1 && pages[0] == heap + 0x3000, "a scan over reserved holes: %llu", (unsigned long long)n);
    CK(plat_mem_decommit(heap, 0x200000) && !plat_mem_read(heap, &byte, 1), "decommit");
    CK(plat_mem_commit(heap, 0x200000) && p[0x5000] == 0, "a recommit reads zeros");
    plat_mem_written(heap, 0x200000, pages, 8, 1);
    p[0x1000] = 1;
    CK(plat_mem_written(heap, 0x200000, pages, 8, 1) == 1, "tracking after a recommit");
    CK(plat_mem_protect(heap, 0x1000, PLAT_PROT_READ) && plat_mem_is_readonly(heap, 0x1000), "read-only");
    CK(plat_mem_protect(heap, 0x1000, PLAT_PROT_RW), "read-write again");
    plat_mem_release(base, 1ull << 30);
    printf("memory: reservation, commit, write tracking: %s\n", fails > before ? "FAIL" : "ok");
}

// ---- files: the calls the save commit stands on
static void probe_files(void) {
    int before = fails;
    char tmp[PLAT_MAX_PATH], dir[PLAT_MAX_PATH + 64], x[PLAT_MAX_PATH + 96], y[PLAT_MAX_PATH + 96], z[PLAT_MAX_PATH + 96];
    CK(plat_temp_dir(tmp, sizeof tmp), "temp directory");
    snprintf(dir, sizeof dir, "%s" PLAT_SEP "totk-probe-plat-%u", tmp, plat_process_id());
    snprintf(x, sizeof x, "%s" PLAT_SEP "x", dir);
    snprintf(y, sizeof y, "%s" PLAT_SEP "y", dir);
    snprintf(z, sizeof z, "%s" PLAT_SEP "z", dir);
    plat_file_mkdir_p(dir);
    CK(plat_path_info(dir) & PLAT_PATH_DIR, "the directory exists");
    CK(!plat_path_info(x) && plat_error_kind(plat_last_error()) == PLAT_ERR_NOT_FOUND, "a missing path reads as not found");
    PlatFile *f = plat_file_open(x, PLAT_FILE_READ | PLAT_FILE_WRITE | PLAT_FILE_CREATE | PLAT_FILE_EXCL);
    CK(f != NULL, "creating a file");
    CK(!plat_file_open(x, PLAT_FILE_WRITE | PLAT_FILE_CREATE | PLAT_FILE_EXCL) &&
       plat_error_kind(plat_last_error()) == PLAT_ERR_EXISTS, "exclusive create of an existing file");
    CK(plat_file_pwrite(f, "hello", 5, 2) == 5, "a positioned write");
    char buf[16] = "";
    CK(plat_file_pread(f, buf, sizeof buf, 0) == 7 && !memcmp(buf + 2, "hello", 5), "a read stopping at the end of the file");
    CK(plat_file_pread(f, buf, 4, 100) == 0, "a read past the end is 0, not an error");
    CK(plat_file_handle_size(f) == 7 && plat_file_set_size(f, 3) && plat_file_handle_size(f) == 3, "file size");
    PlatFileId i1, i2;
    CK(plat_file_identity(f, &i1) && plat_file_identity(f, &i2) && !memcmp(&i1, &i2, sizeof i1), "file identity");
    CK(plat_file_sync(f), "sync");
    plat_file_close(f);
    CK(plat_file_link(x, y) && plat_file_links(x) == 2, "a hard link");
    CK(plat_file_exists(y) && plat_file_delete(y) && !plat_file_exists(y), "deleting the link");
    CK(plat_file_move_durable(x, y) && plat_file_exists(y) && !plat_file_exists(x), "a durable move");
    CK(plat_file_write(x, "new", 3) && !plat_file_move_durable(x, y), "a move onto an existing file fails");
    CK(plat_dir_sync(dir), "a directory sync");
    CK(plat_file_write_atomic(z, "abc", 3), "an atomic write");
    uint64_t size = 0;
    uint8_t *got = plat_file_read(z, &size);
    CK(got && size == 3 && !memcmp(got, "abc", 3), "reading it back");
    free(got);
    CK(plat_file_set_readonly(z, 1) && (plat_path_info(z) & PLAT_PATH_READONLY), "read-only flag");
    CK(!plat_file_write_atomic(z, "xyz", 3), "an atomic write over a read-only file fails");
    plat_file_set_readonly(z, 0);
    int seen = 0, dirs = 0;
    char sub[PLAT_MAX_PATH + 96];
    snprintf(sub, sizeof sub, "%s" PLAT_SEP "sub", dir);
    CK(plat_dir_create(sub) && !plat_dir_create(sub) && plat_dir_make(sub), "creating a directory");
    PlatDir *d = plat_dir_open(dir);
    PlatDirEntry e;
    while (d && plat_dir_next(d, &e)) seen++, dirs += e.is_dir;
    plat_dir_close(d);
    CK(seen == 4 && dirs == 1, "listing: %d entries, %d directories", seen, dirs);
    CK(!plat_dir_remove(dir) && plat_error_kind(plat_last_error()) == PLAT_ERR_NOT_EMPTY, "removing a full directory fails");
    plat_dir_remove(sub), plat_file_delete(x), plat_file_delete(y), plat_file_delete(z);
    CK(plat_dir_remove(dir), "removing the empty directory");
    printf("files: %s\n", fails > before ? "FAIL" : "ok");
}

static void hex32(const uint8_t *d, char *out) {
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

static void probe_misc(void) {
    int before = fails;
    uint8_t d[32];
    char h[65];
    PlatSha256 *s = plat_sha256_new();
    plat_sha256_update(s, "abc", 3);
    plat_sha256_finish(s, d);
    hex32(d, h);
    CK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256 of abc: %s", h);
    s = plat_sha256_new();
    plat_sha256_finish(s, d);
    hex32(d, h);
    CK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "SHA-256 of nothing: %s", h);
    char *mil = malloc(1000000);
    memset(mil, 'a', 1000000);
    s = plat_sha256_new();
    for (int off = 0; off < 1000000; off += 997) plat_sha256_update(s, mil + off, off + 997 > 1000000 ? 1000000 - off : 997);
    plat_sha256_finish(s, d);
    free(mil);
    hex32(d, h);
    CK(!strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"), "SHA-256 of a million a: %s", h);
    uint16_t u[8];
    CK(plat_utf8_to_utf16("a\xc3\xa9\xf0\x9f\x98\x80", u, 8) == 5 && u[1] == 0xe9 && u[2] == 0xd83d && u[3] == 0xde00 && !u[4],
       "UTF-8 to UTF-16");
    CK(!plat_utf8_to_utf16("\xff", u, 8), "invalid UTF-8");
    uint64_t r1 = 0, r2 = 0;
    CK(plat_random(&r1, 8) && plat_random(&r2, 8) && r1 != r2, "random bytes");
    uint64_t t0 = plat_now_ns();
    plat_sleep_ns(2000000);
    uint64_t dt = plat_now_ns() - t0;
    CK(dt >= 2000000 && dt < 50000000, "a 2 ms sleep took %llu ns", (unsigned long long)dt);
    printf("SHA-256, UTF-16, random, sleep: %s\n", fails > before ? "FAIL" : "ok");
}

int probe_platform(void) {
    probe_locks();
    probe_events();
    probe_fibers();
    probe_memory();
    probe_files();
    probe_misc();
    printf("probe-platform: %s (%d failures)\n", fails ? "FAILED" : "passed", fails);
    return fails ? 1 : 0;
}
