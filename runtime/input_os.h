// What input.c asks of the system's keyboard, mouse and cursor (input_win.c, input_posix.c). Any thread.
#pragma once

int input_os_foreground(void);                  // the game window has the keyboard
int input_os_key_down(int key);                 // a key id of input_map.h, mouse buttons included
int input_os_esc_down(void);
int input_os_cursor_captured(void);             // hidden and held for the mouse camera
void input_os_mouse_take(int *dx, int *dy);     // relative motion since the last call; resets it
void input_os_cursor_update(void);              // whether the cursor should be captured may have changed
void input_os_menu_closed(void);                // the capture an Esc gave up comes back

// From input.c to the system's side.
float input_mouse_sens(void);                   // 0: the mouse camera is off
int input_ui_holds_cursor(void);                // a text entry or the menu is open and needs the cursor free
