// The Linux side of input.c (input_os.h), on the SDL3 window of gfx_window_posix.c: keys and mouse buttons as the window
// thread sees them, relative mouse motion for the camera, and the cursor capture.
#include <stdint.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "cfg.h"
#include "input.h"
#include "input_map.h"
#include "input_os.h"

void gfx_window_post_capture(void);   // gfx_window_posix.c: has the window thread call input_posix_apply_capture

// SDL scancode of a key id (the one place that knows them), SDL_SCANCODE_UNKNOWN for a mouse button or an id without a key.
static SDL_Scancode key_scan(int key) {
    if (key >= KEY_A && key <= KEY_Z) return (SDL_Scancode)(SDL_SCANCODE_A + (key - KEY_A));
    if (key == KEY_0) return SDL_SCANCODE_0;
    if (key > KEY_0 && key <= KEY_9) return (SDL_Scancode)(SDL_SCANCODE_1 + (key - KEY_0 - 1));
    if (key == KEY_NUM0) return SDL_SCANCODE_KP_0;
    if (key > KEY_NUM0 && key <= KEY_NUM9) return (SDL_Scancode)(SDL_SCANCODE_KP_1 + (key - KEY_NUM0 - 1));
    switch (key) {
    case KEY_SPACE: return SDL_SCANCODE_SPACE;
    case KEY_TAB: return SDL_SCANCODE_TAB;
    case KEY_LSHIFT: return SDL_SCANCODE_LSHIFT;
    case KEY_LCTRL: return SDL_SCANCODE_LCTRL;
    case KEY_UP: return SDL_SCANCODE_UP;
    case KEY_DOWN: return SDL_SCANCODE_DOWN;
    case KEY_LEFT: return SDL_SCANCODE_LEFT;
    case KEY_RIGHT: return SDL_SCANCODE_RIGHT;
    case KEY_ENTER: return SDL_SCANCODE_RETURN;
    case KEY_BACKSPACE: return SDL_SCANCODE_BACKSPACE;
    case KEY_RSHIFT: return SDL_SCANCODE_RSHIFT;
    case KEY_RCTRL: return SDL_SCANCODE_RCTRL;
    case KEY_LALT: return SDL_SCANCODE_LALT;
    case KEY_CAPSLOCK: return SDL_SCANCODE_CAPSLOCK;
    case KEY_INSERT: return SDL_SCANCODE_INSERT;
    case KEY_DELETE: return SDL_SCANCODE_DELETE;
    case KEY_HOME: return SDL_SCANCODE_HOME;
    case KEY_END: return SDL_SCANCODE_END;
    case KEY_PAGEUP: return SDL_SCANCODE_PAGEUP;
    case KEY_PAGEDOWN: return SDL_SCANCODE_PAGEDOWN;
    case KEY_F2: return SDL_SCANCODE_F2;
    case KEY_F3: return SDL_SCANCODE_F3;
    case KEY_F4: return SDL_SCANCODE_F4;
    case KEY_F5: return SDL_SCANCODE_F5;
    case KEY_F6: return SDL_SCANCODE_F6;
    case KEY_F7: return SDL_SCANCODE_F7;
    case KEY_F9: return SDL_SCANCODE_F9;
    case KEY_F12: return SDL_SCANCODE_F12;
    case KEY_NUM_ADD: return SDL_SCANCODE_KP_PLUS;
    case KEY_NUM_SUB: return SDL_SCANCODE_KP_MINUS;
    case KEY_NUM_MUL: return SDL_SCANCODE_KP_MULTIPLY;
    case KEY_NUM_DIV: return SDL_SCANCODE_KP_DIVIDE;
    case KEY_NUM_DECIMAL: return SDL_SCANCODE_KP_PERIOD;
    case KEY_MINUS: return SDL_SCANCODE_MINUS;
    case KEY_EQUALS: return SDL_SCANCODE_EQUALS;
    case KEY_LBRACKET: return SDL_SCANCODE_LEFTBRACKET;
    case KEY_RBRACKET: return SDL_SCANCODE_RIGHTBRACKET;
    case KEY_SEMICOLON: return SDL_SCANCODE_SEMICOLON;
    case KEY_APOSTROPHE: return SDL_SCANCODE_APOSTROPHE;
    case KEY_COMMA: return SDL_SCANCODE_COMMA;
    case KEY_PERIOD: return SDL_SCANCODE_PERIOD;
    case KEY_SLASH: return SDL_SCANCODE_SLASH;
    case KEY_BACKSLASH: return SDL_SCANCODE_BACKSLASH;
    case KEY_GRAVE: return SDL_SCANCODE_GRAVE;
    }
    return SDL_SCANCODE_UNKNOWN;
}

uint32_t input_key_char(int key) {
    if (key < KEY_MINUS || key > KEY_GRAVE) return 0;
    return SDL_GetKeyFromScancode(key_scan(key), SDL_KMOD_NONE, false);
}

static volatile uint8_t scan_down[SDL_SCANCODE_COUNT];
static volatile uint8_t mouse_down[6];        // SDL_BUTTON_LEFT .. SDL_BUTTON_X2
static volatile int32_t has_keyboard;         // the window has keyboard focus
static volatile int32_t mouse_dx, mouse_dy;   // whole counts since the last take
static float rest_x, rest_y;                  // window thread: the fractions SDL's relative motion left over
static volatile int32_t captured;
static volatile int32_t released;             // Esc: until a click in the window. RELEASE_ESC: closing the menu gives it back
enum { RELEASE = 1, RELEASE_ESC = 2 };
static volatile int32_t suspended;            // input_cursor_suspend

int input_os_foreground(void) { return __atomic_load_n(&has_keyboard, __ATOMIC_SEQ_CST); }

int input_os_key_down(int key) {
    if (key >= KEY_MOUSE_L && key <= KEY_MOUSE_X2) {
        static const uint8_t button[] = {SDL_BUTTON_LEFT, SDL_BUTTON_RIGHT, SDL_BUTTON_MIDDLE, SDL_BUTTON_X1, SDL_BUTTON_X2};
        return mouse_down[button[key - KEY_MOUSE_L]];
    }
    SDL_Scancode s = key_scan(key);
    return s != SDL_SCANCODE_UNKNOWN && scan_down[s];
}

int input_os_esc_down(void) { return scan_down[SDL_SCANCODE_ESCAPE]; }
int input_os_cursor_captured(void) { return __atomic_load_n(&captured, __ATOMIC_SEQ_CST); }

void input_os_mouse_take(int *dx, int *dy) {
    *dx = __atomic_exchange_n(&mouse_dx, 0, __ATOMIC_SEQ_CST);
    *dy = __atomic_exchange_n(&mouse_dy, 0, __ATOMIC_SEQ_CST);
}

void input_os_cursor_update(void) { gfx_window_post_capture(); }

void input_os_menu_closed(void) {
    int32_t o = __atomic_load_n(&released, __ATOMIC_SEQ_CST);
    if (o & RELEASE_ESC) __atomic_store_n(&released, 0, __ATOMIC_SEQ_CST);
}

void input_cursor_suspend(int on) {
    __atomic_store_n(&suspended, on != 0, __ATOMIC_SEQ_CST);
    input_os_cursor_update();
}

int input_cursor_released(void) { return (__atomic_load_n(&released, __ATOMIC_SEQ_CST) & RELEASE) != 0; }

// Window thread: relative mouse mode hides the cursor and, on Wayland, locks the pointer, while the window has the
// keyboard, no text entry or menu needs the cursor, and the mouse camera is on.
void input_posix_apply_capture(SDL_Window *w) {
    const char *in = cfg_get("TOTK_INPUT");
    int want = input_os_foreground() && !(__atomic_load_n(&released, __ATOMIC_SEQ_CST) & RELEASE) &&
               !__atomic_load_n(&suspended, __ATOMIC_SEQ_CST) && !input_ui_holds_cursor() && input_mouse_sens() > 0 &&
               !(in && !strcmp(in, "none"));
    if (want == __atomic_load_n(&captured, __ATOMIC_SEQ_CST)) return;
    SDL_SetWindowRelativeMouseMode(w, want);
    if (want) {
        __atomic_store_n(&mouse_dx, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&mouse_dy, 0, __ATOMIC_SEQ_CST);
        rest_x = rest_y = 0;
    }
    __atomic_store_n(&captured, want, __ATOMIC_SEQ_CST);
}

// Window thread, every event before the menu's adapter sees it.
void input_posix_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (e->key.scancode < SDL_SCANCODE_COUNT) scan_down[e->key.scancode] = e->type == SDL_EVENT_KEY_DOWN;
        // Esc is unbound in the game layout: it releases the cursor and opens the menu (ui/menu_posix.cpp)
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_ESCAPE && !e->key.repeat &&
            !(__atomic_load_n(&released, __ATOMIC_SEQ_CST) & RELEASE)) {
            __atomic_store_n(&released, RELEASE | RELEASE_ESC, __ATOMIC_SEQ_CST);
            input_os_cursor_update();
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.button < sizeof mouse_down) mouse_down[e->button.button] = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && (__atomic_load_n(&released, __ATOMIC_SEQ_CST) & RELEASE)) {
            __atomic_store_n(&released, 0, __ATOMIC_SEQ_CST);   // input.c drops the click itself
            input_os_cursor_update();
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (input_os_cursor_captured()) {
            rest_x += e->motion.xrel;
            rest_y += e->motion.yrel;
            int32_t wx = (int32_t)rest_x, wy = (int32_t)rest_y;
            rest_x -= (float)wx, rest_y -= (float)wy;
            __atomic_add_fetch(&mouse_dx, wx, __ATOMIC_SEQ_CST);
            __atomic_add_fetch(&mouse_dy, wy, __ATOMIC_SEQ_CST);
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        __atomic_store_n(&has_keyboard, 1, __ATOMIC_SEQ_CST);
        input_os_cursor_update();
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:   // releases go elsewhere from here on: nothing stays held
        __atomic_store_n(&has_keyboard, 0, __ATOMIC_SEQ_CST);
        __atomic_store_n(&released, 0, __ATOMIC_SEQ_CST);   // Alt+Tab back in captures again
        memset((void *)scan_down, 0, sizeof scan_down);
        memset((void *)mouse_down, 0, sizeof mouse_down);
        input_os_cursor_update();
        break;
    }
}
