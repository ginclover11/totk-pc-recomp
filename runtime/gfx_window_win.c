// The Win32 window: its thread and message loop, focus, size and fullscreen settings, the monitor's refresh rate. The
// Vulkan side is gfx_present.c. Focus behaviour from MacNativeGames, games/totk/runtime/frontend.m:88-97, 175-176, 350-352
// (https://github.com/ly05010419/MacNativeGames @ b75be8cfa704df3701e86130c8ced268e79d6b11, MIT, Copyright (c) 2026
// MacNativeGames contributors).
//
// Threads: the window thread owns the HWND and its message loop and never runs guest code. WM_SIZE only marks the
// swapchain dirty. Keyboard and mouse for the menu come from the window procedure (ui/menu_win.cpp). The window's size and
// fullscreen state are the settings TOTK_WINDOW_SIZE and TOTK_FULLSCREEN, applied at start and whenever a cfg_set changes
// them.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "cfg.h"
#include "flight.h"
#include "fs.h"
#include "gfx.h"
#include "gfx_window_os.h"
#include "input.h"
#include "nvn_api.h"
#include "ui/menu.h"
#include "vk.h"

// ---- window state
static HWND g_hwnd;
static HANDLE g_wnd_ready;
static volatile LONG g_focused = 1;
static volatile LONG g_focus_msgs;
static int g_wnd_failed;
static int g_background;                   // TOTK_BACKGROUND_WINDOW: the window stays behind the user's windows
static int g_fs_pending;                   // window thread: TOTK_FULLSCREEN waits for the first activation
static volatile LONG g_dpi = USER_DEFAULT_SCREEN_DPI;   // the window's DPI, set by the window thread

// ---- focus
int gfx_window_focused(void) { return g_focused; }
unsigned gfx_window_focus_messages(void) { return (unsigned)g_focus_msgs; }
HWND gfx_window_handle(void) { return g_hwnd; }

// ---- window thread
#define WND_STYLE WS_OVERLAPPEDWINDOW
#define WM_APP_SETTINGS (WM_APP + 1)
#define WM_APP_FULLSCREEN (WM_APP + 2)   // the fullscreen setting, held back until the window is first activated

static WINDOWPLACEMENT g_saved_place = {sizeof g_saved_place};   // the window behind fullscreen (window thread)

static int is_fullscreen(HWND h) { return !(GetWindowLongW(h, GWL_STYLE) & WS_OVERLAPPEDWINDOW); }

unsigned gfx_window_dpi(void) { return (unsigned)InterlockedCompareExchange(&g_dpi, 0, 0); }

// The outer size of a framed window whose client area is cw x ch. The frame is measured at the window's DPI, or the
// system DPI before the window exists: plain AdjustWindowRect always takes the system DPI, which is wrong on a monitor
// at another display scale.
static void outer_size(HWND h, int cw, int ch, int *ow, int *oh) {
    RECT r = {0, 0, cw, ch};
    AdjustWindowRectExForDpi(&r, WND_STYLE, FALSE, 0, h ? GetDpiForWindow(h) : GetDpiForSystem());
    *ow = r.right - r.left;
    *oh = r.bottom - r.top;
}

// TOTK_FULLSCREEN=1: borderless window over the monitor. TOTK_WINDOW_SIZE=<w>x<h>: client area of the window (the
// swapchain follows the window; the game image is scaled to it).
static int cfg_fullscreen(void) {
    return cfg_bool("TOTK_FULLSCREEN");
}

static void cfg_window_size(int *w, int *h) {
    int x = 0, y = 0;
    if (sscanf(cfg_value("TOTK_WINDOW_SIZE"), "%dx%d", &x, &y) != 2 || x < 640 || y < 360 || x > 7680 || y > 4320)
        sscanf(cfg_meta("WINDOW_SIZE")->def, "%dx%d", &x, &y);   // unparsable or out of range: the default
    *w = x;
    *h = y;
}

// Windows turns the display off and sleeps after the idle timeout, and gamepad input does not count as activity. While
// the window is up and not minimized it holds the display and the system on, as other games do. The state belongs to
// the calling thread, so only the window thread (window_main and wnd_proc) calls this; the process exit releases it.
static void keep_awake(int on) {
    static int cur = -1;
    if (on == cur) return;
    cur = on;
    SetThreadExecutionState(on ? ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
}

// F11: borderless window over the whole monitor, and back to the saved window. The swapchain follows through WM_SIZE.
// Not Alt+Enter: Enter is the Plus button (input_win.c). A background window (test runs) keeps its place behind the
// user's windows and takes no focus.
static void toggle_fullscreen(HWND h) {
    LONG style = GetWindowLongW(h, GWL_STYLE);
    if (style & WS_OVERLAPPEDWINDOW) {
        MONITORINFO mi = {sizeof mi};
        if (!GetWindowPlacement(h, &g_saved_place) || !GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi))
            return;
        SetWindowLongW(h, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED | (g_background ? SWP_NOZORDER | SWP_NOACTIVATE : 0));
    } else {
        SetWindowLongW(h, GWL_STYLE, (style & ~WS_POPUP) | WND_STYLE);
        if (g_background) g_saved_place.showCmd = SW_SHOWNOACTIVATE;
        SetWindowPlacement(h, &g_saved_place);
        SetWindowPos(h, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED |
                         (g_background ? SWP_NOACTIVATE : 0));
    }
}

// The window size and fullscreen state last taken from the settings (window thread). apply_settings acts on a change of
// either value only, so a window resized by hand keeps its size when another setting changes.
static char g_applied_size[32];
static int g_applied_fs;

static void note_applied(void) {
    snprintf(g_applied_size, sizeof g_applied_size, "%s", cfg_value("TOTK_WINDOW_SIZE"));
    g_applied_fs = cfg_fullscreen();
}

// Window thread: the window takes the size and fullscreen state of the settings when they changed. In fullscreen the
// size goes into the placement restored when fullscreen ends.
static void apply_settings(HWND h) {
    int size_changed = strcmp(cfg_value("TOTK_WINDOW_SIZE"), g_applied_size) != 0, fs = cfg_fullscreen(), fs_changed = fs != g_applied_fs;
    note_applied();
    if (size_changed) {
        int cw, ch, ow, oh;
        cfg_window_size(&cw, &ch);
        outer_size(h, cw, ch, &ow, &oh);
        if (is_fullscreen(h)) {
            RECT *n = &g_saved_place.rcNormalPosition;
            n->right = n->left + ow;
            n->bottom = n->top + oh;
        } else {
            RECT have;
            if (GetClientRect(h, &have) && (have.right != cw || have.bottom != ch)) {
                if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
                SetWindowPos(h, NULL, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }
    if (fs_changed && fs != is_fullscreen(h)) {
        input_cursor_suspend(1);
        toggle_fullscreen(h);
        input_cursor_suspend(0);
    }
}

void gfx_window_apply_settings(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_SETTINGS, 0, 0);
}

void gfx_window_request_close(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

// ---- refresh rate of the window's monitor (TOTK_FPS_LIMIT=display). The window thread measures it when the window
// opens, lands on another monitor or the display mode changes; readers take the stored value. QueryDisplayConfig gives
// the exact rational rate (59.94, 143.998); EnumDisplaySettings' whole hertz is the fallback.
static HMONITOR g_rate_mon;
static volatile LONG g_rate_mhz;   // millihertz, 0 unknown

static double monitor_rate_hz(HMONITOR mon) {
    MONITORINFOEXW mi = {.cbSize = sizeof mi};
    if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi)) return 0;
    double hz = 0;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS && np && nm) {
        DISPLAYCONFIG_PATH_INFO *paths = calloc(np, sizeof *paths);
        DISPLAYCONFIG_MODE_INFO *modes = calloc(nm, sizeof *modes);
        if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS)
            for (UINT32 i = 0; i < np && !hz; i++) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {.header = {.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
                                                                   .size = sizeof src,
                                                                   .adapterId = paths[i].sourceInfo.adapterId,
                                                                   .id = paths[i].sourceInfo.id}};
                DISPLAYCONFIG_RATIONAL r = paths[i].targetInfo.refreshRate;
                if (DisplayConfigGetDeviceInfo(&src.header) == ERROR_SUCCESS && !wcscmp(src.viewGdiDeviceName, mi.szDevice) &&
                    r.Denominator)
                    hz = (double)r.Numerator / r.Denominator;
            }
        free(paths);
        free(modes);
    }
    DEVMODEW dm = {.dmSize = sizeof dm};
    if (!hz && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        hz = dm.dmDisplayFrequency;   // 0 and 1 mean the hardware default
    return hz;
}

// Window thread. force: the display mode changed, measure even on the same monitor.
static void refresh_rate_update(HWND h, int force) {
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY);
    if (!force && mon == g_rate_mon) return;
    g_rate_mon = mon;
    double hz = monitor_rate_hz(mon);
    LONG mhz = (LONG)(hz * 1000 + 0.5);
    if (InterlockedExchange(&g_rate_mhz, mhz) != mhz) rt_log("gfx: monitor refresh rate %.3f Hz", hz);
}

double gfx_window_refresh_hz(void) { return InterlockedCompareExchange(&g_rate_mhz, 0, 0) / 1000.0; }

int gfx_window_monitor_size(int *w, int *h) {
    MONITORINFO mi = {sizeof mi};
    if (!g_hwnd || !GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) return 0;
    *w = mi.rcMonitor.right - mi.rcMonitor.left;
    *h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    return 1;
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    LRESULT input_res;
    if (input_window_msg(h, m, w, l, &input_res)) return input_res;
    if (menu_window_msg(h, m, w, l, &input_res)) return input_res;
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_F11 && !(l & (1 << 30))) {   // bit 30: auto-repeat
            input_cursor_suspend(1);
            toggle_fullscreen(h);
            input_cursor_suspend(0);
            cfg_set("TOTK_FULLSCREEN", is_fullscreen(h) ? "1" : "0");   // the menu shows it; the next start keeps it
            cfg_save();
            note_applied();   // what cfg_get reads now (the environment may still override it)
        }
        break;
    case WM_APP_SETTINGS:
        apply_settings(h);
        return 0;
    case WM_APP_FULLSCREEN:
        if (cfg_fullscreen() && !is_fullscreen(h)) {
            input_cursor_suspend(1);
            toggle_fullscreen(h);
            input_cursor_suspend(0);
        }
        return 0;
    case WM_SIZE:
        keep_awake(w != SIZE_MINIMIZED);
        if (w != SIZE_MINIMIZED) gfx_present_mark_dirty();
        refresh_rate_update(h, 0);
        return 0;
    case WM_EXITSIZEMOVE:   // dragged onto another monitor
        refresh_rate_update(h, 0);
        break;
    case WM_DISPLAYCHANGE:
        refresh_rate_update(h, 1);
        break;
    case WM_GETDPISCALEDSIZE:   // a framed window keeps its client size in pixels on a monitor at another scale
        if (!is_fullscreen(h) && !IsZoomed(h)) {
            RECT c, r;
            if (GetClientRect(h, &c)) {
                r = (RECT){0, 0, c.right, c.bottom};
                if (AdjustWindowRectExForDpi(&r, WND_STYLE, FALSE, 0, LOWORD(w))) {
                    *(SIZE *)l = (SIZE){r.right - r.left, r.bottom - r.top};
                    return TRUE;
                }
            }
        }
        break;   // DefWindowProc: Windows scales the size with the DPI
    case WM_DPICHANGED: {   // display scale changed, or the window reached a monitor at another scale
        const RECT *s = (const RECT *)l;
        InterlockedExchange(&g_dpi, LOWORD(w));
        rt_log("gfx: window DPI %u (%u%%)", LOWORD(w), LOWORD(w) * 100u / USER_DEFAULT_SCREEN_DPI);
        if (is_fullscreen(h)) {   // keep covering the monitor the suggested rect lies on
            MONITORINFO mi = {sizeof mi};
            if (GetMonitorInfoW(MonitorFromRect(s, MONITOR_DEFAULTTONEAREST), &mi))
                SetWindowPos(h, NULL, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                             mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            SetWindowPos(h, NULL, s->left, s->top, s->right - s->left, s->bottom - s->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_ACTIVATEAPP: {
        int in = w != 0;
        InterlockedExchange(&g_focused, in);
        oe_set_focus(in);                          // pinned to 1 by TOTK_ASSUME_FOCUS inside
        oe_push_message(OE_MSG_FOCUS_STATE_CHANGED);
        InterlockedIncrement(&g_focus_msgs);
        if (in && g_fs_pending) {
            g_fs_pending = 0;
            PostMessageW(h, WM_APP_FULLSCREEN, 0, 0);
        }
        return 0;
    }
    case WM_CLOSE:
        // The process ends with the window, as in MNG, once a save Commit still running in the background is done.
        rt_log("gfx: window closed; exiting");
        fs_save_flush();
        vk_pipeline_save();
        prof_write();   // ExitProcess runs no atexit handlers
        ExitProcess(0);
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI window_main(void *arg) {
    (void)arg;
    HINSTANCE inst = GetModuleHandleW(NULL);
    // Icon resource 1 of totk.rc (runtime/totk.ico): the title bar, the taskbar button and Alt+Tab.
    WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = wnd_proc, .hInstance = inst,
                      .hIcon = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                          GetSystemMetrics(SM_CYICON), LR_SHARED),
                      .hIconSm = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                            GetSystemMetrics(SM_CYSMICON), LR_SHARED),
                      .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .lpszClassName = L"TotKRecompWindow"};
    RegisterClassExW(&wc);
    int cw, ch, ow, oh;
    cfg_window_size(&cw, &ch);
    outer_size(NULL, cw, ch, &ow, &oh);
    // The window opens behind the user's windows without taking focus: it shows the game's blank loading frames for a
    // while, and must not cover what the player works on meanwhile. The game still counts itself in focus until the
    // first WM_ACTIVATEAPP, so loading goes on, and keyboard input waits for the foreground (input_win.c). Its taskbar
    // button flashes until it is brought up, and TOTK_FULLSCREEN waits for that first activation.
    // TOTK_BACKGROUND_WINDOW=1 (test runs): no flashing, never fullscreen at start, and F11 keeps the window's place.
    int background = g_background = cfg_get("TOTK_BACKGROUND_WINDOW") && atoi(cfg_get("TOTK_BACKGROUND_WINDOW"));
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"TotK Recomp", WND_STYLE, CW_USEDEFAULT, CW_USEDEFAULT, ow, oh, NULL,
                             NULL, inst, NULL);
    if (!g_hwnd) {
        rt_log("gfx: CreateWindowEx failed (%lu)", GetLastError());
        g_wnd_failed = 1;
    } else {
        // The frame was measured at the system DPI; the window may have opened on a monitor at another scale.
        InterlockedExchange(&g_dpi, (LONG)GetDpiForWindow(g_hwnd));
        outer_size(g_hwnd, cw, ch, &ow, &oh);
        SetWindowPos(g_hwnd, HWND_BOTTOM, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (!background) {
            FLASHWINFO fw = {.cbSize = sizeof fw, .hwnd = g_hwnd, .dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG};
            FlashWindowEx(&fw);
            g_fs_pending = cfg_fullscreen();
        }
        note_applied();
        refresh_rate_update(g_hwnd, 1);
        keep_awake(1);
    }
    SetEvent(g_wnd_ready);
    if (g_wnd_failed) return 1;
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// ---- what gfx_present.c asks of the window
void gfx_os_open(void) {
    if (g_hwnd) return;
    g_wnd_failed = 0;
    g_wnd_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, window_main, NULL, 0, NULL);
    if (!t) rt_fatal("gfx: cannot start the window thread (error %lu)", GetLastError());
    CloseHandle(t);
    WaitForSingleObject(g_wnd_ready, INFINITE);
    CloseHandle(g_wnd_ready);
    if (g_wnd_failed) rt_fatal("gfx: window creation failed");
}

VkResult gfx_os_create_surface(VkInstance instance, VkSurfaceKHR *out) {
    VkWin32SurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                      .hinstance = GetModuleHandleW(NULL), .hwnd = g_hwnd};
    return vkCreateWin32SurfaceKHR(instance, &si, NULL, out);
}

void gfx_os_client_size(int *w, int *h) {
    RECT rc = {0};
    GetClientRect(g_hwnd, &rc);
    *w = rc.right;
    *h = rc.bottom;
}

int gfx_os_minimized(void) { return IsIconic(g_hwnd) != 0; }

void gfx_os_resize_client(int w, int h) {
    int ow, oh;
    outer_size(g_hwnd, w, h, &ow, &oh);
    SetWindowPos(g_hwnd, NULL, 0, 0, ow, oh, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
