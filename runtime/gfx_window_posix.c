// The Linux window, on SDL3 (its Wayland backend, X11 where there is no compositor): its thread and event loop, focus,
// size and fullscreen settings, the monitor's refresh rate. The Vulkan side is gfx_present.c.
//
// Threads: the window thread creates the window, owns SDL's video and never runs guest code. Other threads ask it for
// changes through user events. The drawable size is kept here, as the swapchain needs it from the present thread.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "cfg.h"
#include "fs.h"
#include "gfx.h"
#include "gfx_window_os.h"
#include "host.h"
#include "input.h"
#include "nvn_api.h"
#include "platform/platform.h"
#include "ui/menu.h"
#include "vk.h"

void input_posix_event(const SDL_Event *e);   // input_posix.c, then the menu's adapter
void input_posix_apply_capture(SDL_Window *w);
void menu_sdl_event(const SDL_Event *e, float pixels_per_unit);   // ui/menu_posix.cpp

enum { USER_SETTINGS, USER_CAPTURE };

static SDL_Window *g_win;
static uint32_t g_user_event;
static PlatEvent *g_ready;
static int g_failed;
static volatile int32_t g_focused = 1, g_focus_msgs, g_minimized, g_px_w, g_px_h;
static volatile int32_t g_rate_mhz, g_mon_w, g_mon_h;   // of the monitor the window is on; 0 unknown
static volatile int32_t g_dpi = 96;

static void post(int code) {
    if (!g_user_event) return;
    SDL_Event e = {.user = {.type = g_user_event, .code = code}};
    SDL_PushEvent(&e);
}

void gfx_window_post_capture(void) { post(USER_CAPTURE); }

static int cfg_fullscreen(void) { return cfg_bool("TOTK_FULLSCREEN"); }

static void cfg_window_size(int *w, int *h) {
    int x = 0, y = 0;
    if (sscanf(cfg_value("TOTK_WINDOW_SIZE"), "%dx%d", &x, &y) != 2 || x < 640 || y < 360 || x > 7680 || y > 4320)
        sscanf(cfg_meta("WINDOW_SIZE")->def, "%dx%d", &x, &y);   // unparsable or out of range: the default
    *w = x;
    *h = y;
}

// The window size and fullscreen state last taken from the settings (window thread): a change of either value acts, so a
// window resized by hand keeps its size when another setting changes.
static char g_applied_size[32];
static int g_applied_fs;

static void note_applied(void) {
    snprintf(g_applied_size, sizeof g_applied_size, "%s", cfg_value("TOTK_WINDOW_SIZE"));
    g_applied_fs = cfg_fullscreen();
}

static int is_fullscreen(void) { return (SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN) != 0; }

static void apply_settings(void) {
    int size_changed = strcmp(cfg_value("TOTK_WINDOW_SIZE"), g_applied_size) != 0, fs = cfg_fullscreen(), fs_changed = fs != g_applied_fs;
    note_applied();
    if (size_changed) {
        int w, h;
        cfg_window_size(&w, &h);
        SDL_SetWindowSize(g_win, w, h);
    }
    if (fs_changed && fs != is_fullscreen()) SDL_SetWindowFullscreen(g_win, fs);
}

void gfx_window_apply_settings(void) { post(USER_SETTINGS); }

void gfx_window_request_close(void) {
    SDL_Event e = {.type = SDL_EVENT_QUIT};
    SDL_PushEvent(&e);
}

// Window thread: the monitor's refresh rate and size, read again when the window moves or the display changes.
static void monitor_update(void) {
    SDL_DisplayID id = SDL_GetDisplayForWindow(g_win);
    const SDL_DisplayMode *m = id ? SDL_GetCurrentDisplayMode(id) : NULL;
    if (!m) return;
    double hz = m->refresh_rate_denominator ? (double)m->refresh_rate_numerator / m->refresh_rate_denominator : (double)m->refresh_rate;
    int32_t mhz = (int32_t)(hz * 1000 + 0.5);
    if (__atomic_exchange_n(&g_rate_mhz, mhz, __ATOMIC_SEQ_CST) != mhz) rt_log("gfx: monitor refresh rate %.3f Hz", hz);
    __atomic_store_n(&g_mon_w, (int32_t)((float)m->w * m->pixel_density), __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_mon_h, (int32_t)((float)m->h * m->pixel_density), __ATOMIC_SEQ_CST);
    float scale = SDL_GetWindowDisplayScale(g_win);
    if (scale > 0) __atomic_store_n(&g_dpi, (int32_t)(scale * 96 + 0.5f), __ATOMIC_SEQ_CST);
}

double gfx_window_refresh_hz(void) { return __atomic_load_n(&g_rate_mhz, __ATOMIC_SEQ_CST) / 1000.0; }

int gfx_window_monitor_size(int *w, int *h) {
    int mw = __atomic_load_n(&g_mon_w, __ATOMIC_SEQ_CST), mh = __atomic_load_n(&g_mon_h, __ATOMIC_SEQ_CST);
    if (!mw || !mh) return 0;
    *w = mw;
    *h = mh;
    return 1;
}

// The menu's text size floor: 96 is 100%, as on Windows.
unsigned gfx_window_dpi(void) { return (unsigned)__atomic_load_n(&g_dpi, __ATOMIC_SEQ_CST); }

int gfx_window_focused(void) { return __atomic_load_n(&g_focused, __ATOMIC_SEQ_CST); }
unsigned gfx_window_focus_messages(void) { return (unsigned)__atomic_load_n(&g_focus_msgs, __ATOMIC_SEQ_CST); }

static void size_update(void) {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_win, &w, &h);
    __atomic_store_n(&g_px_w, w, __ATOMIC_SEQ_CST);
    __atomic_store_n(&g_px_h, h, __ATOMIC_SEQ_CST);
    gfx_present_mark_dirty();
}

static void set_focus(int in) {
    __atomic_store_n(&g_focused, in, __ATOMIC_SEQ_CST);
    oe_set_focus(in);                          // pinned to 1 by TOTK_ASSUME_FOCUS inside
    oe_push_message(OE_MSG_FOCUS_STATE_CHANGED);
    __atomic_add_fetch(&g_focus_msgs, 1, __ATOMIC_SEQ_CST);
}

static void window_event(const SDL_Event *e) {
    if (e->type == g_user_event) {
        if (e->user.code == USER_SETTINGS) apply_settings();
        else input_posix_apply_capture(g_win);
        return;
    }
    input_posix_event(e);
    menu_sdl_event(e, SDL_GetWindowPixelDensity(g_win));
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (e->key.scancode == SDL_SCANCODE_F11 && !e->key.repeat) {   // not Alt+Enter: Enter is the Plus button
            int fs = !is_fullscreen();   // asked now, and on Wayland granted later: the flag cannot be read back here
            SDL_SetWindowFullscreen(g_win, fs);
            cfg_set("TOTK_FULLSCREEN", fs ? "1" : "0");
            cfg_save();
            note_applied();
        }
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        size_update();
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
        __atomic_store_n(&g_minimized, 1, __ATOMIC_SEQ_CST);
        break;
    case SDL_EVENT_WINDOW_RESTORED:
        __atomic_store_n(&g_minimized, 0, __ATOMIC_SEQ_CST);
        gfx_present_mark_dirty();
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        set_focus(1);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        set_focus(0);
        break;
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
    case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
        monitor_update();
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    case SDL_EVENT_QUIT:
        // The process ends with the window, once a save Commit still running in the background is done.
        rt_log("gfx: window closed; exiting");
        fs_save_flush();
        vk_pipeline_save();
        prof_write();
        fflush(stdout);
        fflush(stderr);
        plat_exit_now(0);
    }
}

static void window_main(void *arg) {
    (void)arg;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        rt_log("gfx: SDL video init failed: %s", SDL_GetError());
        g_failed = 1;
        plat_event_set(g_ready);
        return;
    }
    g_user_event = SDL_RegisterEvents(1);
    int w, h;
    cfg_window_size(&w, &h);
    SDL_WindowFlags fl = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (cfg_fullscreen()) fl |= SDL_WINDOW_FULLSCREEN;
    g_win = SDL_CreateWindow("TotK Recomp", w, h, fl);
    if (!g_win) {
        rt_log("gfx: SDL_CreateWindow failed: %s", SDL_GetError());
        g_failed = 1;
        plat_event_set(g_ready);
        return;
    }
    rt_log("gfx: window on SDL's %s video driver", SDL_GetCurrentVideoDriver());
    note_applied();
    monitor_update();
    size_update();
    plat_event_set(g_ready);
    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) window_event(&e);
    }
}

// ---- what gfx_present.c asks of the window
void gfx_os_open(void) {
    if (g_win) return;
    g_ready = plat_event_new(1, 0);
    PlatThread *t = plat_thread_new(window_main, NULL, "window");
    if (!t) rt_fatal("gfx: cannot start the window thread (error %u)", plat_last_error());
    plat_thread_detach(t);
    plat_event_wait(g_ready);
    if (g_failed) rt_fatal("gfx: window creation failed");
}

VkResult gfx_os_create_surface(VkInstance instance, VkSurfaceKHR *out) {
    if (SDL_Vulkan_CreateSurface(g_win, instance, NULL, out)) return VK_SUCCESS;
    rt_log("gfx: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
    return VK_ERROR_INITIALIZATION_FAILED;
}

void gfx_os_client_size(int *w, int *h) {
    *w = __atomic_load_n(&g_px_w, __ATOMIC_SEQ_CST);
    *h = __atomic_load_n(&g_px_h, __ATOMIC_SEQ_CST);
}

int gfx_os_minimized(void) { return __atomic_load_n(&g_minimized, __ATOMIC_SEQ_CST); }
void gfx_os_resize_client(int w, int h) { SDL_SetWindowSize(g_win, w, h); }

// The instance needs the surface extensions before any window exists (probes make the device without one), so it asks
// for every one this loader has rather than SDL's list, which needs the video subsystem running.
uint32_t gfx_window_instance_exts(const char **out, uint32_t max) {
    static const char *const want[] = {VK_KHR_SURFACE_EXTENSION_NAME, "VK_KHR_wayland_surface", "VK_KHR_xcb_surface",
                                       "VK_KHR_xlib_surface"};
    uint32_t n = 0, got = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
    VkExtensionProperties *p = calloc(n ? n : 1, sizeof *p);
    if (p) vkEnumerateInstanceExtensionProperties(NULL, &n, p);
    for (size_t i = 0; p && i < sizeof want / sizeof *want && got < max; i++)
        for (uint32_t k = 0; k < n; k++)
            if (!strcmp(p[k].extensionName, want[i])) out[got++] = want[i];
    free(p);
    return got;
}

// Any graphics family presents on Wayland, the compositor copes; gfx_present.c checks the surface it gets.
int gfx_window_present_support(VkInstance instance, VkPhysicalDevice pd, uint32_t family) {
    (void)instance, (void)pd, (void)family;
    return 1;
}

// --probe-window: the window opens and the swapchain follows it. The Win32 probe (gfx_window_probe_win.c) goes further.
int probe_window(void) {
    gfx_window_start();
    int w = 0, h = 0;
    gfx_os_client_size(&w, &h);
    plat_sleep_ms(200);
    printf("probe-window: %dx%d, present mode %s, refresh %.3f Hz, focused %d\n", w, h, gfx_window_present_mode(),
           gfx_window_refresh_hz(), gfx_window_focused());
    int ok = w > 0 && h > 0 && gfx_window_present_mode()[0];
    printf("probe-window: %s\n", ok ? "passed" : "FAILED");
    return !ok;
}

// --probe-dpi checks Windows' DPI awareness (dpi_probe_win.c); nothing to check here, the swapchain is in pixels.
int probe_dpi(void) {
    printf("probe-dpi: skipped: Windows only\n");
    return 0;
}

// RenderDoc capture (rdoc_win.c): architecture.md says it cannot capture this renderer anyway.
void rdoc_init(void) {}
int rdoc_active(void) { return 0; }
void rdoc_after_present(uint64_t frame) { (void)frame; }
