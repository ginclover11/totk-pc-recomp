// Linux implementation of platform.h: pthreads, CLOCK_MONOTONIC, futex locks, mmap, PAGEMAP_SCAN write tracking.
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <link.h>
#include <linux/fs.h>
#include <linux/futex.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <xmmintrin.h>

#include <SDL3/SDL_clipboard.h>

#include "platform.h"

extern char **environ;

#define PAGE 4096ull
#define DEFAULT_STACK (16ull << 20)   // the size win32 builds give the exe with --stack

#define XPATH 4096

// Paths may come with either separator; the kernel wants forward slashes. 0 when the path does not fit.
static int unixify(const char *path, char *out) {
    size_t n = strlen(path);
    if (n >= XPATH) return 0;
    for (size_t i = 0; i <= n; i++) out[i] = path[i] == '\\' ? '/' : path[i];
    return 1;
}

// ---- threads

uint32_t plat_thread_id(void) {
    static __thread uint32_t tid;
    if (!tid) tid = (uint32_t)syscall(SYS_gettid);
    return tid;
}

struct PlatThread {
    pthread_t h;
    PlatThreadFn fn;
    void *arg;
    char name[16];
    int joined;             // a timed wait already collected the thread
    _Atomic uint32_t tid;   // its kernel id, once it has started
};

static void *thread_main(void *p) {
    PlatThread *t = p;
    atomic_store(&t->tid, plat_thread_id());
    if (t->name[0]) pthread_setname_np(pthread_self(), t->name);
    t->fn(t->arg);
    return NULL;
}

PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    return plat_thread_new_stack(fn, arg, name, 0);
}

PlatThread *plat_thread_new_stack(PlatThreadFn fn, void *arg, const char *name, size_t stack_bytes) {
    PlatThread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    if (name) snprintf(t->name, sizeof t->name, "%s", name);   // the kernel keeps 15 bytes of a name
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_bytes ? stack_bytes : DEFAULT_STACK);
    int e = pthread_create(&t->h, &attr, thread_main, t);
    pthread_attr_destroy(&attr);
    if (e) { free(t); errno = e; return NULL; }
    return t;
}

void plat_thread_join(PlatThread *t) {
    if (!t->joined) pthread_join(t->h, NULL);
    free(t);
}

int plat_thread_wait_ms(PlatThread *t, uint32_t ms) {
    if (t->joined) return 1;
    struct timespec at;
    clock_gettime(CLOCK_REALTIME, &at);
    uint64_t ns = (uint64_t)at.tv_nsec + (uint64_t)ms * 1000000ull;
    at.tv_sec += (time_t)(ns / 1000000000ull);
    at.tv_nsec = (long)(ns % 1000000000ull);
    if (pthread_timedjoin_np(t->h, NULL, &at)) return 0;
    t->joined = 1;
    return 1;
}

// The thread keeps using t->fn and t->arg, so the struct stays allocated (a few bytes per detached thread).
void plat_thread_detach(PlatThread *t) { pthread_detach(t->h); }

uint64_t plat_thread_affinity(PlatThread *t, uint64_t mask) {
    pthread_t h = t ? t->h : pthread_self();
    cpu_set_t old, set;
    if (pthread_getaffinity_np(h, sizeof old, &old)) return 0;
    CPU_ZERO(&set);
    for (int i = 0; i < 64; i++) if (mask >> i & 1) CPU_SET(i, &set);
    if (pthread_setaffinity_np(h, sizeof set, &set)) return 0;
    uint64_t prev = 0;
    for (int i = 0; i < 64; i++) if (CPU_ISSET(i, &old)) prev |= 1ull << i;
    return prev;
}

uint32_t plat_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (uint32_t)n : 1;
}

void plat_thread_name(const char *name) {
    char n[16];
    snprintf(n, sizeof n, "%s", name);
    pthread_setname_np(pthread_self(), n);
}

// Niceness per thread, the only knob an unprivileged Linux process has. Raising needs RLIMIT_NICE, so a higher level
// usually stays as it was; lowering always works.
void plat_thread_priority(PlatThread *t, int level) {
    uint32_t tid = 0;
    if (t) while (!(tid = atomic_load(&t->tid))) sched_yield();   // it has just started
    setpriority(PRIO_PROCESS, tid, level <= PLAT_PRIO_IDLE ? 19 : -5 * level);
}

// The kernel's clock id for a thread's CPU time: the bit pattern of its ABI (CPUCLOCK_SCHED, per thread).
uint64_t plat_thread_cpu_ns(uint32_t tid) {
    struct timespec t;
    if (!tid || clock_gettime((clockid_t)((~(int)tid << 3) | 6), &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

// One real-time signal, past the two glibc keeps for itself. SA_RESTART: a system call the signal catches in a host
// library goes on; the waiter only sends it to a thread it saw in game code, but that can have changed by delivery.
// SA_ONSTACK: game code may keep data below its stack pointer, and a stack frame there would destroy it.
#define INTERRUPT_SIGNAL (SIGRTMIN + 6)
static PlatInterruptFn g_interrupt;

static void interrupt_handler(int sig, siginfo_t *si, void *uc_) {
    (void)sig, (void)si;
    ucontext_t *uc = uc_;
    int saved = errno;
    uint64_t to = g_interrupt((uint64_t)uc->uc_mcontext.gregs[REG_RIP], NULL);
    if (to) uc->uc_mcontext.gregs[REG_RIP] = (greg_t)to;
    errno = saved;
}

void plat_interrupt_init(PlatInterruptFn fn) {
    g_interrupt = fn;
    struct sigaction sa = {.sa_sigaction = interrupt_handler, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART};
    sigemptyset(&sa.sa_mask);
    sigaction(INTERRUPT_SIGNAL, &sa, NULL);
}

PlatThreadRef *plat_thread_ref_self(void) { return (PlatThreadRef *)(uintptr_t)plat_thread_id(); }
void plat_thread_ref_close(PlatThreadRef *r) { (void)r; }

int plat_thread_interrupt(PlatThreadRef *r, void *arg) {
    (void)arg;
    return syscall(SYS_tgkill, getpid(), (pid_t)(uintptr_t)r, INTERRUPT_SIGNAL) ? PLAT_INT_FAILED : PLAT_INT_SENT;
}

// Fibers. makecontext and swapcontext would do, at a system call per switch for the signal mask. plat_fiber_swap keeps
// what the System V ABI has a callee preserve, plus the FP control words, on the stack it leaves.
struct PlatFiber {
    void *rsp;
    void *stack;
    size_t size;
};

static __thread PlatFiber *g_fiber;

void plat_fiber_swap(void **save_rsp, void *load_rsp);
__asm__(".text\n"
        ".globl plat_fiber_swap\n"
        ".type plat_fiber_swap, @function\n"
        ".p2align 4\n"
        "plat_fiber_swap:\n"
        "    pushq %rbp\n"
        "    pushq %rbx\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    subq $8, %rsp\n"
        "    stmxcsr (%rsp)\n"
        "    fnstcw 4(%rsp)\n"
        "    movq %rsp, (%rdi)\n"
        "    movq %rsi, %rsp\n"
        "    ldmxcsr (%rsp)\n"
        "    fldcw 4(%rsp)\n"
        "    addq $8, %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbx\n"
        "    popq %rbp\n"
        "    ret\n"
        ".size plat_fiber_swap, .-plat_fiber_swap\n"
        // A new fiber's first swap returns here with fn in r12 and its argument in r13.
        ".type plat_fiber_start, @function\n"
        "plat_fiber_start:\n"
        "    movq %r13, %rdi\n"
        "    callq *%r12\n"
        "    ud2\n"
        ".size plat_fiber_start, .-plat_fiber_start\n");
extern char plat_fiber_start[] __asm__("plat_fiber_start");

PlatFiber *plat_fiber_convert(void) {
    g_fiber = calloc(1, sizeof *g_fiber);
    return g_fiber;
}

void plat_fiber_unconvert(void) {
    free(g_fiber);
    g_fiber = NULL;
}

PlatFiber *plat_fiber_new(size_t stack_bytes, void (*fn)(void *arg), void *arg) {
    PlatFiber *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->size = stack_bytes;
    f->stack = mmap(NULL, stack_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (f->stack == MAP_FAILED) {
        free(f);
        return NULL;
    }
    mprotect(f->stack, PAGE, PROT_NONE);   // the guard page
    // The frame plat_fiber_swap pops, from the lowest address: FP control words, r15, r14, r13, r12, rbx, rbp, return
    // address. The stack ends 16-byte aligned once that return address is popped, as the start's call expects.
    uint64_t *top = (uint64_t *)(((uintptr_t)f->stack + stack_bytes) & ~(uintptr_t)15);
    uint64_t *sp = top - 8;
    uint32_t mxcsr = 0x1f80;
    uint16_t x87 = 0x037f;
    memcpy((char *)sp, &mxcsr, 4);
    memcpy((char *)sp + 4, &x87, 2);
    sp[1] = 0;                               // r15
    sp[2] = 0;                               // r14
    sp[3] = (uint64_t)(uintptr_t)arg;        // r13
    sp[4] = (uint64_t)(uintptr_t)fn;         // r12
    sp[5] = 0;                               // rbx
    sp[6] = 0;                               // rbp
    sp[7] = (uint64_t)(uintptr_t)plat_fiber_start;
    f->rsp = sp;
    return f;
}

void plat_fiber_switch(PlatFiber *to) {
    PlatFiber *from = g_fiber;
    g_fiber = to;
    plat_fiber_swap(&from->rsp, to->rsp);
}

void plat_fiber_delete(PlatFiber *f) {
    if (f->stack) munmap(f->stack, f->size);
    free(f);
}

int plat_thread_priority_get(void) {
    errno = 0;
    int nice_value = getpriority(PRIO_PROCESS, 0);
    return errno || nice_value >= 19 ? PLAT_PRIO_IDLE : -nice_value / 5;
}

void plat_yield(void) { sched_yield(); }

// ---- time

uint64_t plat_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

void plat_sleep_ns(uint64_t ns) {
    uint64_t end = plat_now_ns() + ns;
    struct timespec t = {(time_t)(end / 1000000000ull), (long)(end % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) {}
}

void plat_wait_ns(uint64_t ns) { plat_sleep_ns(ns); }
void plat_sleep_ms(uint32_t ms) { plat_sleep_ns((uint64_t)ms * 1000000ull); }
uint64_t plat_counter(void) { return plat_now_ns(); }
uint64_t plat_counter_hz(void) { return 1000000000ull; }
void plat_timer_fine(void) {}   // Linux timers are already fine-grained

uint64_t plat_tick_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &t);
    return (uint64_t)t.tv_sec * 1000ull + (uint64_t)t.tv_nsec / 1000000ull;
}

// ---- locks and condition variables: futexes on the first 32 bits of the zero-initialised storage

_Static_assert(sizeof(PlatMutex) >= 2 * sizeof(uint32_t), "PlatMutex holds the state and the count of shared waiters");

#define L_WRITER 0x80000000u
#define L_WAITERS 0x40000000u   // someone may sleep: the unlock must wake
#define L_READERS 0x3fffffffu
#define L_SPINS 64

typedef struct { _Atomic uint32_t state, shared_waiting; } Lock;

static long futex(_Atomic uint32_t *w, int op, uint32_t val, const struct timespec *to) {
    return syscall(SYS_futex, w, op | FUTEX_PRIVATE_FLAG, val, to, NULL, 0);
}

// Wakes every sleeper when one may be a reader (they can all go on), else one writer.
static void lock_wake(Lock *l) {
    futex(&l->state, FUTEX_WAKE, atomic_load(&l->shared_waiting) ? INT_MAX : 1, NULL);
}

void plat_mutex_init(PlatMutex *m) { memset(m, 0, sizeof *m); }

int plat_mutex_trylock(PlatMutex *m) {
    Lock *l = (Lock *)m;
    uint32_t s = 0;
    return atomic_compare_exchange_strong(&l->state, &s, L_WRITER);
}

void plat_mutex_lock(PlatMutex *m) {
    Lock *l = (Lock *)m;
    uint32_t s = 0;
    if (atomic_compare_exchange_strong(&l->state, &s, L_WRITER)) return;
    for (int i = 0; i < L_SPINS; i++) {
        s = 0;
        if (atomic_load_explicit(&l->state, memory_order_relaxed) == 0 &&
            atomic_compare_exchange_strong(&l->state, &s, L_WRITER))
            return;
        _mm_pause();
    }
    for (;;) {
        s = atomic_load(&l->state);
        if (!(s & (L_WRITER | L_READERS))) {
            // Taken after possibly sleeping: keep the flag, other sleepers may remain, and the unlock wakes the next.
            if (atomic_compare_exchange_strong(&l->state, &s, L_WRITER | L_WAITERS)) return;
            continue;
        }
        if (!(s & L_WAITERS) && !atomic_compare_exchange_strong(&l->state, &s, s | L_WAITERS)) continue;
        futex(&l->state, FUTEX_WAIT, s | L_WAITERS, NULL);
    }
}

void plat_mutex_unlock(PlatMutex *m) {
    Lock *l = (Lock *)m;
    if (atomic_exchange(&l->state, 0) & L_WAITERS) lock_wake(l);
}

int plat_mutex_trylock_shared(PlatMutex *m) {
    Lock *l = (Lock *)m;
    uint32_t s = atomic_load(&l->state);
    while (!(s & (L_WRITER | L_WAITERS)))
        if (atomic_compare_exchange_weak(&l->state, &s, s + 1)) return 1;
    return 0;
}

void plat_mutex_lock_shared(PlatMutex *m) {
    Lock *l = (Lock *)m;
    if (plat_mutex_trylock_shared(m)) return;
    atomic_fetch_add(&l->shared_waiting, 1);
    int slept = 0;
    for (;;) {
        uint32_t s = atomic_load(&l->state);
        // A newcomer queues behind sleepers, so readers cannot starve a writer; one already woken goes on.
        if (!(s & L_WRITER) && (slept || !(s & L_WAITERS))) {
            if (atomic_compare_exchange_strong(&l->state, &s, s + 1)) break;
            continue;
        }
        if (!(s & L_WAITERS) && !atomic_compare_exchange_strong(&l->state, &s, s | L_WAITERS)) continue;
        futex(&l->state, FUTEX_WAIT, s | L_WAITERS, NULL);
        slept = 1;
    }
    atomic_fetch_sub(&l->shared_waiting, 1);
}

void plat_mutex_unlock_shared(PlatMutex *m) {
    Lock *l = (Lock *)m;
    uint32_t s = atomic_fetch_sub(&l->state, 1) - 1;
    // The last reader with sleepers. A failed exchange means someone took the lock, flag included: its unlock wakes.
    if (s == L_WAITERS && atomic_compare_exchange_strong(&l->state, &s, 0)) lock_wake(l);
}

typedef struct { _Atomic uint32_t seq; } Cond;
_Static_assert(sizeof(PlatCond) >= sizeof(Cond), "PlatCond holds the sequence");

void plat_cond_init(PlatCond *c) { memset(c, 0, sizeof *c); }

void plat_cond_wait(PlatCond *c, PlatMutex *m) {
    Cond *v = (Cond *)c;
    uint32_t seq = atomic_load(&v->seq);
    plat_mutex_unlock(m);
    futex(&v->seq, FUTEX_WAIT, seq, NULL);
    plat_mutex_lock(m);
}

int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
    Cond *v = (Cond *)c;
    uint32_t seq = atomic_load(&v->seq);
    struct timespec to = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    plat_mutex_unlock(m);
    long r = futex(&v->seq, FUTEX_WAIT, seq, &to);
    int timed_out = r < 0 && errno == ETIMEDOUT;
    plat_mutex_lock(m);
    return !timed_out;
}

void plat_cond_signal(PlatCond *c) {
    Cond *v = (Cond *)c;
    atomic_fetch_add(&v->seq, 1);
    futex(&v->seq, FUTEX_WAKE, 1, NULL);
}

void plat_cond_broadcast(PlatCond *c) {
    Cond *v = (Cond *)c;
    atomic_fetch_add(&v->seq, 1);
    futex(&v->seq, FUTEX_WAKE, INT_MAX, NULL);
}

// 0 not run, 1 running, 2 done.
void plat_once(PlatOnce *o, void (*fn)(void)) {
    _Atomic uint32_t *w = (_Atomic uint32_t *)o;
    uint32_t s = atomic_load(w);
    if (s == 2) return;
    s = 0;
    if (atomic_compare_exchange_strong(w, &s, 1)) {
        fn();
        atomic_store(w, 2);
        futex(w, FUTEX_WAKE, INT_MAX, NULL);
        return;
    }
    while ((s = atomic_load(w)) != 2) futex(w, FUTEX_WAIT, s, NULL);
}

struct PlatEvent {
    _Atomic uint32_t set;
    int manual;
};

PlatEvent *plat_event_new(int manual, int initially_set) {
    PlatEvent *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->manual = manual;
    e->set = initially_set != 0;
    return e;
}

void plat_event_free(PlatEvent *e) { free(e); }

void plat_event_set(PlatEvent *e) {
    if (!e) return;   // as SetEvent fails on a null handle: callers may set an event that was never made
    atomic_store(&e->set, 1);
    futex(&e->set, FUTEX_WAKE, e->manual ? INT_MAX : 1, NULL);
}

void plat_event_reset(PlatEvent *e) { atomic_store(&e->set, 0); }

static int event_take(PlatEvent *e) {
    if (e->manual) return atomic_load(&e->set) != 0;
    uint32_t one = 1;
    return atomic_compare_exchange_strong(&e->set, &one, 0);
}

void plat_event_wait(PlatEvent *e) {
    while (!event_take(e)) futex(&e->set, FUTEX_WAIT, 0, NULL);
}

int plat_event_wait_ms(PlatEvent *e, uint32_t ms) {
    if (ms == PLAT_WAIT_FOREVER) { plat_event_wait(e); return 1; }
    uint64_t end = plat_now_ns() + (uint64_t)ms * 1000000ull;
    for (;;) {
        if (event_take(e)) return 1;
        uint64_t now = plat_now_ns();
        if (now >= end) return 0;
        uint64_t ns = end - now;
        struct timespec to = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
        futex(&e->set, FUTEX_WAIT, 0, &to);
    }
}

// ---- memory

// Write tracking is the kernel's: pages of a watched range are write-protected with userfaultfd in its asynchronous
// mode, where a write (from this process or from the kernel on its behalf, as in read(2)) just clears the protection
// and needs no signal or handler thread, and PAGEMAP_SCAN lists and re-protects the written ones. The kernel made this
// pair to emulate GetWriteWatch. User-mode-only userfaultfd needs no privilege, whatever vm.unprivileged_userfaultfd is.
#define MAX_WATCHED 8
static int g_uffd = -1, g_pagemap = -1;
static struct { uint64_t base, size; } g_watched[MAX_WATCHED];
static int g_nwatched;

static int watch_init(void) {
    if (g_uffd >= 0) return 1;
    int fd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (fd < 0) return 0;
    struct uffdio_api api = {.api = UFFD_API, .features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED};
    g_pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (ioctl(fd, UFFDIO_API, &api) || g_pagemap < 0) { close(fd); return 0; }
    g_uffd = fd;
    return 1;
}

static int is_watched(uint64_t addr, size_t n) {
    for (int i = 0; i < g_nwatched; i++)
        if (addr >= g_watched[i].base && addr - g_watched[i].base + n <= g_watched[i].size) return 1;
    return 0;
}

int plat_mem_reserve(uint64_t addr, size_t n, int watch) {
    void *p = mmap((void *)(uintptr_t)addr, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                   -1, 0);
    if (p == MAP_FAILED) return 0;
    if (p != (void *)(uintptr_t)addr) {   // a kernel older than MAP_FIXED_NOREPLACE treats the address as a hint
        munmap(p, n);
        return 0;
    }
    if (watch) {
        struct uffdio_register reg = {.range = {addr, n}, .mode = UFFDIO_REGISTER_MODE_WP};
        if (!watch_init() || g_nwatched == MAX_WATCHED || ioctl(g_uffd, UFFDIO_REGISTER, &reg)) {
            munmap(p, n);
            return 0;
        }
        g_watched[g_nwatched].base = addr;
        g_watched[g_nwatched++].size = n;
    }
    return 1;
}

void plat_mem_release(uint64_t addr, size_t n) { munmap((void *)(uintptr_t)addr, n); }

// Marks pages clean, tracked from here on. The kernel builds page tables for them, so only committed ranges are armed:
// the whole 70 GiB of the watched reservations would take 140 MiB of tables. A commit therefore also forgets earlier
// writes to pages already committed in the range, which plat_mem_written's callers do not depend on: the range is new.
static int watch_arm(uint64_t addr, size_t n) {
    struct uffdio_writeprotect wp = {.range = {addr, n}, .mode = UFFDIO_WRITEPROTECT_MODE_WP};
    return !ioctl(g_uffd, UFFDIO_WRITEPROTECT, &wp);
}

int plat_mem_commit(uint64_t addr, size_t n) {
    if (mprotect((void *)(uintptr_t)addr, n, PROT_READ | PROT_WRITE)) return 0;
    return !is_watched(addr, n) || watch_arm(addr, n);
}

int plat_mem_decommit(uint64_t addr, size_t n) {
    void *p = (void *)(uintptr_t)addr;
    return !madvise(p, n, MADV_DONTNEED) && !mprotect(p, n, PROT_NONE);
}

int plat_mem_protect(uint64_t addr, size_t n, int prot) {
    static const int unix_prot[] = {PROT_NONE, PROT_READ, PROT_READ | PROT_WRITE};
    return !mprotect((void *)(uintptr_t)addr, n, unix_prot[prot]);
}

uint64_t plat_mem_written(uint64_t addr, size_t n, uint64_t *pages, uint64_t max, int reset) {
    if (!is_watched(addr, n)) return UINT64_MAX;
    uint64_t count = 0, at = addr, end = addr + n;
    struct page_region r[256];
    while (at < end && count < max) {
        struct pm_scan_arg a = {.size = sizeof a, .start = at, .end = end, .vec = (uint64_t)(uintptr_t)r, .vec_len = 256,
                                .max_pages = max - count, .category_mask = PAGE_IS_WRITTEN,
                                // A reserved hole has no protection marker, which reads as written, but no contents.
                                .category_anyof_mask = PAGE_IS_PRESENT | PAGE_IS_SWAPPED,
                                .return_mask = PAGE_IS_WRITTEN};
        if (reset) a.flags = PM_SCAN_WP_MATCHING;
        long got = ioctl(g_pagemap, PAGEMAP_SCAN, &a);
        if (got < 0) return UINT64_MAX;
        for (long i = 0; i < got; i++)
            for (uint64_t p = r[i].start; p < r[i].end && count < max; p += PAGE) pages[count++] = p;
        if (a.walk_end <= at) break;
        at = a.walk_end;
    }
    return count;
}

int plat_mem_can_freeze(void) { return 0; }
int plat_mem_freeze(uint64_t addr, size_t n) { (void)addr, (void)n; return 0; }
int plat_mem_thaw(uint64_t addr, size_t n) { (void)addr, (void)n; return 1; }

void *plat_mem_alloc(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

void plat_mem_free(void *p, size_t n) { if (p) munmap(p, n); }

void *plat_aligned_alloc(size_t n, size_t align) {
    void *p;
    return posix_memalign(&p, align, n) ? NULL : p;
}

void plat_aligned_free(void *p) { free(p); }

int plat_mem_is_rw(uint64_t addr) { (void)addr; return 0; }

int plat_mem_is_readonly(uint64_t addr, size_t n) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512], perm[8];
    unsigned long long lo, hi;
    int ro = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perm) == 3 && addr >= lo && addr < hi) {
            ro = perm[0] == 'r' && perm[1] == '-' && addr + n <= hi;
            break;
        }
    fclose(f);
    return ro;
}

int plat_mem_read(uint64_t addr, void *out, size_t n) {
    struct iovec local = {out, n}, remote = {(void *)(uintptr_t)addr, n};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)n;
}

// ---- faults and crashes

static int (*g_write_fault)(uint64_t);
static void (*g_crash)(const PlatCrash *);
static uint64_t g_image_base, g_image_size;

static int image_phdr(struct dl_phdr_info *info, size_t size, void *ctx) {
    (void)size, (void)ctx;
    g_image_base = info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD && ph->p_vaddr + ph->p_memsz > g_image_size) g_image_size = ph->p_vaddr + ph->p_memsz;
    }
    return 1;   // the first entry is the executable
}

uint64_t plat_image_base(void) {
    if (!g_image_size) dl_iterate_phdr(image_phdr, NULL);
    return g_image_base;
}

uint64_t plat_image_size(void) {
    plat_image_base();
    return g_image_size;
}

// 64 KiB holds a signal frame with the full AVX-512 state and the crash report's C calls.
#define SIGNAL_STACK (64ull << 10)

void plat_thread_signals(void) {
    stack_t st = {.ss_sp = mmap(NULL, SIGNAL_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0),
                  .ss_size = SIGNAL_STACK};
    if (st.ss_sp != MAP_FAILED) sigaltstack(&st, NULL);
}

// Fatal signals run on the thread's alternate stack when it has one (plat_thread_signals), so a stack overflow can
// still report; other threads have the kernel use their own stack.
static void fault_handler(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    uint64_t rip = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    int write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
    int errno_saved = errno;
    if (sig == SIGSEGV && si->si_code == SEGV_ACCERR && write && g_write_fault && g_write_fault((uint64_t)si->si_addr)) {
        errno = errno_saved;
        return;
    }
    if (g_crash && rip - plat_image_base() < plat_image_size()) {
        const char *what = sig == SIGSEGV ? "access violation" : sig == SIGBUS ? "in-page error"
                         : sig == SIGILL ? "illegal instruction" : sig == SIGFPE ? "integer divide by zero" : "fault";
        PlatCrash c = {.what = what, .rip = rip, .access = -1};
        if (sig == SIGSEGV) {
            c.addr = (uint64_t)si->si_addr;
            c.access = uc->uc_mcontext.gregs[REG_ERR] & 16 ? 8 : write;
        }
        g_crash(&c);
        _exit(71);
    }
    // Not our code: the default action, which ends the process with this signal when the instruction runs again.
    signal(sig, SIG_DFL);
}

static void faults_install(void) {
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
    struct sigaction sa = {.sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER};
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, NULL);
}

void plat_on_write_fault(int (*fn)(uint64_t addr)) { g_write_fault = fn; faults_install(); }
void plat_on_crash(void (*fn)(const PlatCrash *c)) { g_crash = fn; faults_install(); }
void plat_exit_now(int code) { _exit(code); }

// Writing to a child's closed pipe gives an error here, as on Windows, not a signal.
__attribute__((constructor)) static void ignore_sigpipe(void) { signal(SIGPIPE, SIG_IGN); }

// argv of this process, from the kernel: pointers into one malloc'd buffer, NULL-terminated. NULL on failure.
static char **self_argv(void) {
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (!f) return NULL;
    char *all = NULL;
    size_t used = 0, room = 0;
    for (;;) {
        if (used == room) {
            char *m = realloc(all, room = room ? room * 2 : 4096);
            if (!m) break;
            all = m;
        }
        size_t got = fread(all + used, 1, room - used, f);
        if (!got) break;
        used += got;
    }
    fclose(f);
    size_t count = 0;
    for (size_t i = 0; i < used; i += strlen(all + i) + 1) count++;
    char **argv = malloc((count + 1) * sizeof *argv);
    if (!argv) return NULL;
    count = 0;
    for (size_t i = 0; i < used; i += strlen(all + i) + 1) argv[count++] = all + i;
    argv[count] = NULL;
    return argv;
}

int plat_run_self(char *out, size_t cap, uint32_t *code) {
    char **argv = self_argv();
    int p[2];
    if (!argv || pipe2(p, O_CLOEXEC)) return 0;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_adddup2(&fa, p[1], 2);
    pid_t pid;
    int e = posix_spawn(&pid, "/proc/self/exe", &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    if (e) {
        close(p[0]);
        errno = e;
        return 0;
    }
    size_t n = 0;
    for (;;) {
        char sink[512];
        size_t room = n + 1 < cap ? cap - 1 - n : 0;
        ssize_t k = room ? read(p[0], out + n, room) : read(p[0], sink, sizeof sink);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        if (room) n += (size_t)k;
    }
    out[n] = 0;
    close(p[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    *code = WIFEXITED(status) ? (uint32_t)WEXITSTATUS(status) : 128u + (uint32_t)WTERMSIG(status);
    return 1;
}

void plat_rerun_self(void) {
    char **argv = self_argv();
    if (argv) execv("/proc/self/exe", argv);
}

int plat_setenv(const char *name, const char *value) { return value ? !setenv(name, value, 1) : !unsetenv(name); }

int plat_full_path(const char *path, char *out, size_t cap) {
    char p[XPATH], full[PATH_MAX];
    if (!unixify(path, p)) return 0;
    // A missing path (a directory about to be created) has no realpath; then cwd + path, ".." and all, which the
    // kernel resolves the same way.
    if (!realpath(p, full)) {
        if (p[0] == '/') snprintf(full, sizeof full, "%s", p);
        else {
            char cwd[PATH_MAX / 2];
            if (!getcwd(cwd, sizeof cwd)) return 0;
            snprintf(full, sizeof full, "%s/%.*s", cwd, (int)(PATH_MAX / 2 - 2), p);
        }
    }
    size_t n = strlen(full);
    if (n >= cap) return 0;
    memcpy(out, full, n + 1);
    return 1;
}

long plat_timezone_west(void) {
    tzset();
    return timezone;
}

int plat_temp_dir(char *out, size_t cap) {
    const char *d = getenv("TMPDIR");
    if (!d || !*d) d = "/tmp";
    size_t n = strlen(d);
    while (n > 1 && d[n - 1] == '/') n--;
    if (n >= cap) return 0;
    memcpy(out, d, n);
    out[n] = 0;
    return 1;
}
uint32_t plat_last_error(void) { return (uint32_t)errno; }

static uint64_t g_process_start;
__attribute__((constructor)) static void process_start_note(void) { g_process_start = plat_now_ns(); }
uint64_t plat_process_age_ms(void) { return (plat_now_ns() - g_process_start) / 1000000; }

const char *plat_locale_name(void) { return plat_ui_language(); }
uint32_t plat_process_id(void) { return (uint32_t)getpid(); }

int plat_random(void *out, size_t n) {
    char *p = out;
    while (n) {
        ssize_t k = getrandom(p, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        p += k;
        n -= (size_t)k;
    }
    return 1;
}

// FIPS 180-4.
struct PlatSha256 {
    uint32_t h[8];
    uint64_t total;
    uint8_t buf[64];
    uint32_t used;
};

static const uint32_t sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(uint32_t *st, const uint8_t *b) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)b[i * 4] << 24 | b[i * 4 + 1] << 16 | b[i * 4 + 2] << 8 | b[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = st[0], b2 = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b2) ^ (a & c) ^ (b2 & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b2, b2 = a, a = t1 + t2;
    }
    st[0] += a, st[1] += b2, st[2] += c, st[3] += d, st[4] += e, st[5] += f, st[6] += g, st[7] += h;
}

PlatSha256 *plat_sha256_new(void) {
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    PlatSha256 *s = calloc(1, sizeof *s);
    if (s) memcpy(s->h, init, sizeof init);
    return s;
}

void plat_sha256_update(PlatSha256 *s, const void *p, uint64_t n) {
    const uint8_t *b = p;
    s->total += n;
    if (s->used) {
        uint32_t k = 64 - s->used < n ? 64 - s->used : (uint32_t)n;
        memcpy(s->buf + s->used, b, k);
        s->used += k, b += k, n -= k;
        if (s->used < 64) return;
        sha_block(s->h, s->buf);
        s->used = 0;
    }
    for (; n >= 64; b += 64, n -= 64) sha_block(s->h, b);
    if (n) memcpy(s->buf, b, n), s->used = (uint32_t)n;
}

void plat_sha256_finish(PlatSha256 *s, uint8_t out[32]) {
    uint64_t bits = s->total * 8;
    uint8_t pad[72] = {0x80};
    uint32_t k = (s->used < 56 ? 56 : 120) - s->used;
    plat_sha256_update(s, pad, k);
    for (int i = 0; i < 8; i++) pad[i] = (uint8_t)(bits >> (56 - 8 * i));
    plat_sha256_update(s, pad, 8);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(s->h[i] >> (24 - 8 * j));
    free(s);
}

uint64_t plat_rss(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    unsigned long long size, rss = 0;
    if (f) {
        if (fscanf(f, "%llu %llu", &size, &rss) != 2) rss = 0;
        fclose(f);
    }
    return rss * PAGE;
}

// What the kernel has promised the process and the system, and its limit, from /proc: the nearest to Windows' commit charge.
void plat_commit_info(double *proc_mb, double *used_mb, double *limit_mb) {
    *proc_mb = *used_mb = *limit_mb = -1;
    char line[128];
    unsigned long long kb;
    FILE *f = fopen("/proc/meminfo", "r");
    while (f && fgets(line, sizeof line, f)) {
        if (sscanf(line, "Committed_AS: %llu", &kb) == 1) *used_mb = (double)kb / 1024;
        if (sscanf(line, "CommitLimit: %llu", &kb) == 1) *limit_mb = (double)kb / 1024;
    }
    if (f) fclose(f);
    f = fopen("/proc/self/status", "r");
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "VmData: %llu", &kb) == 1) *proc_mb = (double)kb / 1024;
    if (f) fclose(f);
}

uint64_t plat_peak_rss(void) {
    struct rusage u;
    return getrusage(RUSAGE_SELF, &u) ? 0 : (uint64_t)u.ru_maxrss * 1024;
}

// ---- files

int plat_file_exists(const char *path) {
    char p[XPATH];
    struct stat st;
    return unixify(path, p) && !stat(p, &st) && !S_ISDIR(st.st_mode);
}

int plat_dir_exists(const char *path) {
    char p[XPATH];
    struct stat st;
    return unixify(path, p) && !stat(p, &st) && S_ISDIR(st.st_mode);
}

void plat_file_mkdir_p(const char *path) {
    char tmp[XPATH];
    if (!unixify(path, tmp)) return;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

static int write_all(int fd, const void *data, size_t n) {
    const char *p = data;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        p += k;
        n -= (size_t)k;
    }
    return 1;
}

uint8_t *plat_file_read(const char *path, uint64_t *size) {
    char p[XPATH];
    if (!unixify(path, p)) return NULL;
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    uint8_t *buf = NULL;
    if (!fstat(fd, &st) && st.st_size > 0 && st.st_size < (1ll << 30)) {
        buf = malloc((size_t)st.st_size + 1);
        size_t got = 0;
        while (buf && got < (size_t)st.st_size) {
            ssize_t k = read(fd, buf + got, (size_t)st.st_size - got);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) { free(buf); buf = NULL; break; }
            got += (size_t)k;
        }
        if (buf) {
            buf[st.st_size] = 0;
            *size = (uint64_t)st.st_size;
        }
    }
    close(fd);
    return buf;
}

int plat_file_write(const char *path, const void *data, size_t n) {
    char p[XPATH];
    if (!unixify(path, p)) return 0;
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) return 0;
    int ok = write_all(fd, data, n);
    return !close(fd) && ok;
}

PlatAppend *plat_append_open(const char *path) {
    char p[XPATH];
    if (!unixify(path, p)) return NULL;
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0666);
    return fd < 0 ? NULL : (PlatAppend *)(intptr_t)(fd + 1);   // + 1: a valid descriptor 0 is not NULL
}

int plat_append_write(PlatAppend *f, const void *data, size_t n) { return write_all((int)(intptr_t)f - 1, data, n); }

// The directory entry of a rename reaches the disk when the parent is flushed.
static void sync_parent(const char *path) {
    char d[XPATH];
    snprintf(d, sizeof d, "%s", path);
    char *cut = strrchr(d, '/');
    if (cut) *cut = 0;
    int fd = open(cut ? (d[0] ? d : "/") : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) { fsync(fd); close(fd); }
}

int plat_file_replace(const char *from, const char *to) {
    char a[XPATH], b[XPATH];
    if (!unixify(from, a) || !unixify(to, b) || rename(a, b)) return 0;
    sync_parent(b);
    return 1;
}

int plat_file_write_atomic(const char *path, const void *data, size_t n) {
    char p[XPATH], tmp[XPATH + 48];
    if (!unixify(path, p)) return 0;
    // A rename replaces a read-only file here, where Windows refuses; the read-only flag is the user's "keep this".
    if (plat_path_info(p) & PLAT_PATH_READONLY) {
        errno = EACCES;
        return 0;
    }
    int len = snprintf(tmp, sizeof tmp, "%s.%u.%u.tmp", p, plat_process_id(), plat_thread_id());
    if (len < 0 || len >= (int)sizeof tmp) return 0;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) return 0;
    int ok = write_all(fd, data, n) && !fsync(fd);
    if (close(fd)) ok = 0;
    if (ok && !rename(tmp, p)) {
        sync_parent(p);
        return 1;
    }
    unlink(tmp);
    return 0;
}

int plat_file_flush(const char *path) {
    char p[XPATH];
    if (!unixify(path, p)) return 0;
    int fd = open(p, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int ok = !fsync(fd);
    close(fd);
    return ok;
}

int plat_file_delete(const char *path) {
    char p[XPATH];
    return unixify(path, p) && !unlink(p);
}

int plat_file_truncate(const char *path, uint64_t size) {
    char p[XPATH];
    return unixify(path, p) && !truncate(p, (off_t)size);
}

int64_t plat_file_size(const char *path) {
    char p[XPATH];
    struct stat st;
    return unixify(path, p) && !stat(p, &st) ? (int64_t)st.st_size : -1;
}

int plat_file_set_readonly(const char *path, int on) {
    char p[XPATH];
    struct stat st;
    if (!unixify(path, p) || stat(p, &st)) return 0;
    return !chmod(p, on ? st.st_mode & ~0222u : st.st_mode | 0200u);
}

int plat_file_list(const char *dir, const char *ext, int (*cb)(const char *name, void *ctx), void *ctx) {
    char p[XPATH];
    if (!unixify(dir, p)) return 0;
    DIR *d = opendir(p);
    if (!d) return 0;
    size_t le = strlen(ext);
    for (struct dirent *e; (e = readdir(d));) {
        size_t ln = strlen(e->d_name);
        if (ln < le || strcmp(e->d_name + ln - le, ext) || e->d_type == DT_DIR) continue;
        if (!cb(e->d_name, ctx)) break;
    }
    closedir(d);
    return 1;
}

int plat_dir_make(const char *path) {
    char p[XPATH];
    return unixify(path, p) && (!mkdir(p, 0777) || errno == EEXIST);
}

int plat_dir_create(const char *path) {
    char p[XPATH];
    return unixify(path, p) && !mkdir(p, 0777);
}

struct PlatDir {
    DIR *d;
};

PlatDir *plat_dir_open(const char *dir) {
    char p[XPATH];
    if (!unixify(dir, p)) return NULL;
    DIR *h = opendir(p);
    if (!h) return NULL;
    PlatDir *d = malloc(sizeof *d);
    if (d) d->d = h;
    else closedir(h);
    return d;
}

int plat_dir_next(PlatDir *d, PlatDirEntry *e) {
    for (struct dirent *x; (x = readdir(d->d));) {
        if (!strcmp(x->d_name, ".") || !strcmp(x->d_name, "..")) continue;
        struct stat st, ln;
        if (fstatat(dirfd(d->d), x->d_name, &st, 0)) continue;
        e->name = x->d_name;
        e->is_dir = S_ISDIR(st.st_mode);
        e->is_link = !fstatat(dirfd(d->d), x->d_name, &ln, AT_SYMLINK_NOFOLLOW) && S_ISLNK(ln.st_mode);
        e->size = (uint64_t)st.st_size;
        return 1;
    }
    return 0;
}

void plat_dir_close(PlatDir *d) {
    if (!d) return;
    closedir(d->d);
    free(d);
}

// A link to a directory goes as a link: plat_path_info reports it, and callers never recurse into it.
int plat_dir_remove(const char *path) {
    char p[XPATH];
    struct stat st;
    if (!unixify(path, p) || lstat(p, &st)) return 0;
    return S_ISLNK(st.st_mode) ? !unlink(p) : !rmdir(p);
}

int plat_file_move_durable(const char *from, const char *to) {
    char a[XPATH], b[XPATH];
    if (!unixify(from, a) || !unixify(to, b) || renameat2(AT_FDCWD, a, AT_FDCWD, b, RENAME_NOREPLACE)) return 0;
    sync_parent(b);
    sync_parent(a);
    return 1;
}

int plat_file_link(const char *existing, const char *lnk) {
    char a[XPATH], b[XPATH];
    return unixify(existing, a) && unixify(lnk, b) && !link(a, b);
}

int plat_dir_sync(const char *path) {
    char p[XPATH];
    if (!unixify(path, p)) return 0;
    int fd = open(p, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return 0;
    int ok = !fsync(fd);
    close(fd);
    return ok;
}

int plat_path_info(const char *path) {
    char p[XPATH];
    struct stat ln, st;
    if (!unixify(path, p) || lstat(p, &ln)) return 0;
    int link = S_ISLNK(ln.st_mode), dir = link ? !stat(p, &st) && S_ISDIR(st.st_mode) : S_ISDIR(ln.st_mode);
    return PLAT_PATH_EXISTS | (dir ? PLAT_PATH_DIR : 0) | (link ? PLAT_PATH_LINK : 0) |
           (!(ln.st_mode & 0222) ? PLAT_PATH_READONLY : 0);
}

int plat_error_kind(uint32_t error) {
    switch (error) {
    case ENOENT: case ENOTDIR: return PLAT_ERR_NOT_FOUND;
    case EEXIST: return PLAT_ERR_EXISTS;
    case ENOTEMPTY: return PLAT_ERR_NOT_EMPTY;
    case EBUSY: case ETXTBSY: return PLAT_ERR_BUSY;
    case EFAULT: return PLAT_ERR_BAD_BUFFER;
    default: return PLAT_ERR_OTHER;
    }
}

int plat_file_move(const char *from, const char *to) {
    char a[XPATH], b[XPATH];
    return unixify(from, a) && unixify(to, b) && !renameat2(AT_FDCWD, a, AT_FDCWD, b, RENAME_NOREPLACE);
}

FILE *plat_fopen(const char *path, const char *mode) {
    char p[XPATH];
    return unixify(path, p) ? fopen(p, mode) : NULL;
}

int plat_file_sync_stream(FILE *f) { return !fsync(fileno(f)); }
int plat_fseek(FILE *f, int64_t at, int whence) { return fseeko(f, (off_t)at, whence); }
int64_t plat_ftell(FILE *f) { return ftello(f); }

PlatFile *plat_file_open(const char *path, int flags) {
    char p[XPATH];
    if (!unixify(path, p)) { errno = ENAMETOOLONG; return NULL; }
    int rw = flags & PLAT_FILE_READ && flags & PLAT_FILE_WRITE ? O_RDWR : flags & PLAT_FILE_WRITE ? O_WRONLY : O_RDONLY;
    int fd = open(p, rw | O_CLOEXEC | (flags & PLAT_FILE_CREATE ? O_CREAT : 0) | (flags & PLAT_FILE_TRUNC ? O_TRUNC : 0) |
                         (flags & PLAT_FILE_UNBUFFERED ? O_DIRECT : 0) | (flags & PLAT_FILE_EXCL ? O_CREAT | O_EXCL : 0), 0666);
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

void plat_file_close(PlatFile *f) { if (f) close((int)(intptr_t)f - 1); }

int64_t plat_file_pread(PlatFile *f, void *buf, uint64_t n, uint64_t at) {
    uint64_t done = 0;
    while (done < n) {
        ssize_t k = pread((int)(intptr_t)f - 1, (char *)buf + done, n - done, (off_t)(at + done));
        if (k < 0 && errno == EINTR) continue;
        if (k < 0) return -1;
        if (!k) break;
        done += (uint64_t)k;
    }
    return (int64_t)done;
}

int64_t plat_file_pwrite(PlatFile *f, const void *buf, uint64_t n, uint64_t at) {
    uint64_t done = 0;
    while (done < n) {
        ssize_t k = pwrite((int)(intptr_t)f - 1, (const char *)buf + done, n - done, (off_t)(at + done));
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        done += (uint64_t)k;
    }
    return (int64_t)done;
}

int64_t plat_file_handle_size(PlatFile *f) {
    struct stat st;
    return fstat((int)(intptr_t)f - 1, &st) ? -1 : (int64_t)st.st_size;
}

int plat_file_set_size(PlatFile *f, uint64_t n) { return !ftruncate((int)(intptr_t)f - 1, (off_t)n); }
int plat_file_sync(PlatFile *f) { return !fsync((int)(intptr_t)f - 1); }

int plat_file_identity(PlatFile *f, PlatFileId *id) {
    struct stat st;
    if (fstat((int)(intptr_t)f - 1, &st)) return 0;
    *id = (PlatFileId){(uint32_t)st.st_dev, 0, (uint64_t)st.st_ino, (uint64_t)st.st_size,
                       (uint64_t)st.st_mtim.tv_sec * 1000000000ull + (uint64_t)st.st_mtim.tv_nsec,
                       (uint64_t)st.st_ctim.tv_sec * 1000000000ull + (uint64_t)st.st_ctim.tv_nsec};
    return 1;
}

const void *plat_file_map(PlatFile *f) {
    int64_t size = plat_file_handle_size(f);
    if (size <= 0) return NULL;
    void *v = mmap(NULL, (size_t)size, PROT_READ, MAP_SHARED, (int)(intptr_t)f - 1, 0);
    return v == MAP_FAILED ? NULL : v;
}

int plat_file_links(const char *path) {
    char p[XPATH];
    struct stat st;
    return unixify(path, p) && !stat(p, &st) ? (int)st.st_nlink : 0;
}

int plat_exe_path(char *out, size_t cap) {
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) return 0;
    out[n] = 0;
    return 1;
}

size_t plat_getenv(const char *name, char *out, size_t cap) {
    const char *v = getenv(name);
    size_t n = v ? strlen(v) : 0;
    if (!n || n >= cap) return 0;
    memcpy(out, v, n + 1);
    return n;
}


// "es_AR.UTF-8" from the usual variables, as "es-AR".
const char *plat_ui_language(void) {
    static char tag[32];
    static int done;
    if (!done) {
        const char *e = getenv("LC_ALL");
        if (!e || !*e) e = getenv("LC_MESSAGES");
        if (!e || !*e) e = getenv("LANG");
        tag[0] = 0;
        if (e && strcmp(e, "C") && strcmp(e, "POSIX")) {
            size_t i = 0;
            for (; e[i] && e[i] != '.' && e[i] != '@' && i < sizeof tag - 1; i++) tag[i] = e[i] == '_' ? '-' : e[i];
            tag[i] = 0;
        }
        done = 1;
    }
    return tag;
}

int plat_font_path(const char *lang, char *out, size_t cap) {
    static const char *const cjk[] = {"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                                      "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                                      "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc", NULL};
    if (strcmp(lang, "ja") && strcmp(lang, "ko") && strcmp(lang, "zh-Hans") && strcmp(lang, "zh-Hant")) return 0;
    for (int i = 0; cjk[i]; i++)
        if (plat_file_exists(cjk[i]) && strlen(cjk[i]) < cap) {
            memcpy(out, cjk[i], strlen(cjk[i]) + 1);
            return 1;
        }
    return 0;
}

int plat_os_version(char *out, size_t cap) {
    struct utsname u;
    if (uname(&u)) return 0;
    snprintf(out, cap, "%s %s", u.sysname, u.release);
    return 1;
}

int plat_stderr_path(char *out, size_t cap) {
    struct stat st;
    if (fstat(2, &st) || !S_ISREG(st.st_mode)) return 0;
    ssize_t n = readlink("/proc/self/fd/2", out, cap - 1);
    if (n <= 0) return 0;
    out[n] = 0;
    return 1;
}

// ---- desktop

// Needs SDL's video subsystem, which the game window starts; before that it fails and returns 0.
int plat_clipboard_set(const char *utf8) { return SDL_SetClipboardText(utf8) ? 1 : 0; }

static void reap_child(void *pid) { while (waitpid((pid_t)(intptr_t)pid, NULL, 0) < 0 && errno == EINTR) {} }

int plat_open_folder(const char *path) {
    char p[XPATH];
    if (!unixify(path, p) || !plat_dir_exists(p)) return 0;
    char *const argv[] = {(char *)"xdg-open", p, NULL};
    pid_t pid;
    posix_spawnattr_t a;
    posix_spawnattr_init(&a);
    sigset_t def;
    sigemptyset(&def);
    sigaddset(&def, SIGPIPE);
    posix_spawnattr_setsigdefault(&a, &def);
    posix_spawnattr_setflags(&a, POSIX_SPAWN_SETSIGDEF);
    int e = posix_spawnp(&pid, "xdg-open", NULL, &a, argv, environ);
    posix_spawnattr_destroy(&a);
    if (e) return 0;
    // The shell can take a while to answer and the caller is the present path: a thread reaps it.
    PlatThread *t = plat_thread_new(reap_child, (void *)(intptr_t)pid, "reap");
    if (t) plat_thread_detach(t);
    return 1;
}

// ---- dynamic libraries

void *plat_lib_open(const char *name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
void *plat_lib_sym(void *lib, const char *name) { return lib ? dlsym(lib, name) : NULL; }

// ---- machine load: /proc for the CPU and memory, /sys/class/thermal for the temperature. No GPU numbers yet (-1).

struct PlatLoad {
    uint64_t busy, total, proc;   // the previous call's jiffies; total 0 before the first
};

PlatLoad *plat_load_open(const uint8_t *gpu_luid) {
    (void)gpu_luid;
    return calloc(1, sizeof(PlatLoad));
}

void plat_load_close(PlatLoad *l) { free(l); }

void plat_cpu_load(PlatLoad *l, float *total_pct, float *proc_pct, float *temp_c) {
    *total_pct = *proc_pct = *temp_c = -1;
    if (!l) return;
    unsigned long long v[8] = {0};
    FILE *f = fopen("/proc/stat", "r");
    int got = f ? fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", v, v + 1, v + 2, v + 3, v + 4, v + 5, v + 6, v + 7) : 0;
    if (f) fclose(f);
    struct rusage u;
    if (got == 8 && !getrusage(RUSAGE_SELF, &u)) {
        uint64_t total = 0;
        for (int i = 0; i < 8; i++) total += v[i];
        uint64_t idle = v[3] + v[4], busy = total - idle;
        long hz = sysconf(_SC_CLK_TCK);
        uint64_t proc = ((uint64_t)u.ru_utime.tv_sec + (uint64_t)u.ru_stime.tv_sec) * (uint64_t)hz +
                        ((uint64_t)u.ru_utime.tv_usec + (uint64_t)u.ru_stime.tv_usec) * (uint64_t)hz / 1000000u;
        if (l->total && total > l->total) {
            *total_pct = 100.0f * (float)(busy - l->busy) / (float)(total - l->total);
            *proc_pct = 100.0f * (float)(proc - l->proc) / (float)(total - l->total);
        }
        l->busy = busy, l->total = total, l->proc = proc;
    }
    for (int i = 0; i < 16; i++) {   // the hottest zone, in millidegrees
        char p[64];
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", i);
        FILE *t = fopen(p, "r");
        if (!t) break;
        long m;
        if (fscanf(t, "%ld", &m) == 1 && m / 1000.0f > *temp_c && m < 150000) *temp_c = (float)m / 1000.0f;
        fclose(t);
    }
}

void plat_gpu_load(PlatLoad *l, float *load_pct, float *vram_used_mb) {
    (void)l;
    *load_pct = *vram_used_mb = -1;
}

void plat_mem_info(float *used_mb, float *total_mb, float *proc_mb) {
    *used_mb = *total_mb = *proc_mb = -1;
    long total = sysconf(_SC_PHYS_PAGES), avail = sysconf(_SC_AVPHYS_PAGES), ps = sysconf(_SC_PAGE_SIZE);
    if (total > 0 && avail >= 0) {
        *total_mb = (float)((uint64_t)total * (uint64_t)ps >> 20);
        *used_mb = (float)((uint64_t)(total - avail) * (uint64_t)ps >> 20);
    }
    FILE *f = fopen("/proc/self/statm", "r");
    unsigned long long size, rss;
    if (f) {
        if (fscanf(f, "%llu %llu", &size, &rss) == 2) *proc_mb = (float)(rss * (uint64_t)ps >> 20);
        fclose(f);
    }
}

// ---- child processes

struct PlatChild {
    pid_t pid;
    int in, out;   // in: our write end, out: our read end
};

PlatChild *plat_child_start(const char *exe, const char *arg, uint32_t *err) {
    char p[XPATH];
    if (err) *err = 0;
    if (!unixify(exe, p)) return NULL;
    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC)) return NULL;
    if (pipe2(out, O_CLOEXEC)) {
        close(in[0]), close(in[1]);
        return NULL;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t a;
    posix_spawnattr_init(&a);
    sigset_t def, none;
    sigemptyset(&none);
    sigfillset(&def);
    posix_spawnattr_setsigdefault(&a, &def);
    posix_spawnattr_setsigmask(&a, &none);
    posix_spawnattr_setflags(&a, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
    char *const argv[] = {p, (char *)arg, NULL};
    pid_t pid = 0;
    int e = posix_spawn(&pid, p, &fa, &a, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&a);
    close(in[0]);
    close(out[1]);
    PlatChild *c = e ? NULL : malloc(sizeof *c);
    if (!c) {
        if (!e) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); }
        close(in[1]), close(out[0]);
        if (err) *err = (uint32_t)e;
        return NULL;
    }
    c->pid = pid, c->in = in[1], c->out = out[0];
    return c;
}

void plat_child_stop(PlatChild *c) {
    if (!c) return;
    close(c->in);
    close(c->out);
    kill(c->pid, SIGKILL);
    while (waitpid(c->pid, NULL, 0) < 0 && errno == EINTR) {}
    free(c);
}

int plat_child_write(PlatChild *c, const void *data, size_t n) { return write_all(c->in, data, n); }

long plat_child_read(PlatChild *c, void *buf, size_t cap) {
    for (;;) {
        ssize_t k = read(c->out, buf, cap);
        if (k < 0 && errno == EINTR) continue;
        return (long)k;
    }
}
