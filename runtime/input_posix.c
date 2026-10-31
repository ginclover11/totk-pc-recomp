// Linux stand-in for input_win.c: reports no input until the window is ported (see gfx_window_posix.c).
#include <math.h>
#include <string.h>

#include "input.h"
#include "input_map.h"

void input_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) { *buttons = 0, *lx = *ly = *rx = *ry = 0; }
void input_poll_ui(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) { input_poll(buttons, lx, ly, rx, ry); }
void input_menu(int open) { (void)open; }
int input_capture(int reset, int *id) { (void)reset, (void)id; return 0; }
uint32_t input_key_char(int key) { (void)key; return 0; }
void input_map_snapshot(InputMap *m) { memset(m, 0, sizeof *m); }
int input_pad_read(uint64_t *buttons, int32_t *lx, int32_t *ly) { *buttons = 0, *lx = *ly = 0; return 0; }
int input_pad_layout(void) { return 0; }
int input_pad_label(int pad) { (void)pad; return 0; }
int input_rumble(uint16_t low, uint16_t high, uint32_t ms) { (void)low, (void)high, (void)ms; return 0; }
int input_rumble_triggers_enabled(void) { return 0; }
void input_motion_start(void) {}
int input_motion_enabled(void) { return 0; }

uint16_t rumble_motor(float a, float curve, float strength) {
    if (!(a > 0) || !(strength > 0)) return 0;
    float v = powf(a > 1 ? 1 : a, curve) * strength * 65535.0f + 0.5f;
    return v >= 65535.0f ? 65535 : (uint16_t)v;
}

// SDL's axes to the Switch's; the same mapping as input_win.c. 9.80665: SDL_STANDARD_GRAVITY.
void input_motion_axes(const float a[3], const float g[3], double acc[3], double gyro[3]) {
    acc[0] = -a[0] / 9.80665f;
    acc[1] = a[2] / 9.80665f;
    acc[2] = -a[1] / 9.80665f;
    gyro[0] = g[0];
    gyro[1] = -g[2];
    gyro[2] = g[1];
}
void input_block(int on) { (void)on; }
