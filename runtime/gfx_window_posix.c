// Linux stand-in for gfx_window_win.c. No window yet: the game needs one (a later step ports this onto SDL3, with its
// Wayland backend), and until then asking for a window ends the run with a message that says so. The modes that need
// no window (--selftest and the probes without a display) work.
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"

#define VSYNC_NS 1000000000ull
#define VSYNC_HZ 60
#define NO_WINDOW rt_fatal("%s: the Linux window is not written yet", __func__)

void gfx_window_start(void) { NO_WINDOW; }

// ---- vsync: the game's 60 Hz, on a thread of its own (the same schedule as gfx_window_win.c)
static PlatMutex g_vs_lock = PLAT_MUTEX_INIT;
static uint64_t g_vs_ev[16];
static int g_vs_nev;
static volatile long *g_vs_ctr[4];
static int g_vs_nctr;
static uint64_t g_ticks, g_vs_start_ns;
static int g_vs_started;

static void vsync_main(void *arg) {
    (void)arg;
    plat_thread_priority(NULL, PLAT_PRIO_HIGHEST);
    uint64_t n = 0;
    for (;;) {
        uint64_t start = __atomic_load_n(&g_vs_start_ns, __ATOMIC_RELAXED);
        uint64_t due = start + (n + 1) * VSYNC_NS / VSYNC_HZ, now = kern_now_ns();
        if (now > due + 100000000ull)   // the process stalled: restart the schedule, no catch-up burst
            __atomic_store_n(&g_vs_start_ns, now - (n + 1) * VSYNC_NS / VSYNC_HZ, __ATOMIC_RELAXED);
        else if (due > now)
            plat_wait_ns(due - now);
        n++;
        __atomic_store_n(&g_ticks, n, __ATOMIC_SEQ_CST);
        uint64_t ev[16];
        volatile long *ctr[4];
        plat_mutex_lock_shared(&g_vs_lock);
        int ne = g_vs_nev, nc = g_vs_nctr;
        memcpy(ev, g_vs_ev, sizeof ev);
        memcpy(ctr, g_vs_ctr, sizeof ctr);
        plat_mutex_unlock_shared(&g_vs_lock);
        for (int i = 0; i < nc; i++) __atomic_add_fetch(ctr[i], 1, __ATOMIC_SEQ_CST);
        for (int i = 0; i < ne; i++) kern_signal_system_event(ev[i]);
    }
}

// g_vs_lock held.
static void vsync_ensure(void) {
    if (g_vs_started) return;
    g_vs_started = 1;
    __atomic_store_n(&g_vs_start_ns, kern_now_ns(), __ATOMIC_RELAXED);
    PlatThread *h = plat_thread_new(vsync_main, NULL, "vsync");
    if (!h) rt_fatal("gfx: cannot start the vsync thread (error %u)", plat_last_error());
    plat_thread_detach(h);
}

void gfx_vsync_bind(uint64_t system_event) {
    plat_mutex_lock(&g_vs_lock);
    int have = 0;
    for (int i = 0; i < g_vs_nev; i++) have |= g_vs_ev[i] == system_event;
    if (!have) {
        if (g_vs_nev == 16) rt_fatal("gfx: more than 16 vsync events");
        g_vs_ev[g_vs_nev++] = system_event;
    }
    vsync_ensure();
    plat_mutex_unlock(&g_vs_lock);
}

void gfx_vsync_bind_counter(volatile long *counter) {
    plat_mutex_lock(&g_vs_lock);
    if (g_vs_nctr == 4) rt_fatal("gfx: more than 4 vsync counters");
    g_vs_ctr[g_vs_nctr++] = counter;
    vsync_ensure();
    plat_mutex_unlock(&g_vs_lock);
}

uint64_t gfx_vsync_ticks(void) { return __atomic_load_n(&g_ticks, __ATOMIC_SEQ_CST); }
uint64_t gfx_vsync_time_ns(uint64_t tick) {
    return __atomic_load_n(&g_vs_start_ns, __ATOMIC_RELAXED) + tick * VSYNC_NS / VSYNC_HZ;
}

double gfx_window_refresh_hz(void) { return 0; }
int gfx_window_focused(void) { return 0; }
uint32_t gfx_window_instance_exts(const char **out, uint32_t max) { (void)out, (void)max; return 0; }
int gfx_window_present_support(VkInstance instance, VkPhysicalDevice pd, uint32_t family) {
    (void)instance, (void)pd, (void)family;
    return 1;   // nothing to present to yet; the probes that run without a window only need the queue
}
int probe_window(void) { rt_logf("probe-window: skipped: the Linux window is not written yet\n"); return 0; }

int gfx_window_monitor_size(int *w, int *h) { (void)w, (void)h; return 0; }
const char *gfx_window_present_mode(void) { return "none"; }
void gfx_window_release_vk(void) {}
void gfx_window_request_close(void) {}

// Submissions and presents share the queue; the same recursive lock as gfx_window_win.c.
static PlatRecMutex g_queue_lock;
void vk_queue_lock(void) { plat_rmutex_lock(&g_queue_lock); }
void vk_queue_unlock(void) { plat_rmutex_unlock(&g_queue_lock); }

void vk_present_blit(VkImage src, VkImageLayout layout, VkFormat fmt, int w, int h, const int crop[4]) {
    (void)src, (void)layout, (void)fmt, (void)w, (void)h, (void)crop;
    NO_WINDOW;
}
void vk_present_capture(int on) { (void)on; }
int vk_present_captured(const uint8_t **px, int *w, int *h, VkFormat *fmt) { (void)px, (void)w, (void)h, (void)fmt; return 0; }
void vk_present_flip(void) { NO_WINDOW; }
void vk_thumbs_enable(int n) { (void)n; }
void vk_thumbs_frame(uint64_t frame) { (void)frame; }
int vk_thumbs_read(uint64_t frame, uint8_t *rgba) { (void)frame, (void)rgba; return 0; }

// RenderDoc capture (rdoc_win.c): architecture.md says it cannot capture this renderer anyway.
void rdoc_init(void) {}
int rdoc_active(void) { return 0; }
void rdoc_after_present(uint64_t frame) { (void)frame; }
