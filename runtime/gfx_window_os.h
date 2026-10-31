// What gfx_present.c asks of the system's window (gfx_window_win.c, gfx_window_posix.c).
#pragma once
#include <vulkan/vulkan.h>

void gfx_os_open(void);                             // creates the window once and returns when it exists; fatal on failure
VkResult gfx_os_create_surface(VkInstance instance, VkSurfaceKHR *out);
void gfx_os_client_size(int *w, int *h);            // the drawable area in pixels; any thread
int gfx_os_minimized(void);                         // nothing to present to; any thread
void gfx_os_resize_client(int w, int h);            // programmatic resize
void gfx_present_mark_dirty(void);                  // from the window side: the swapchain no longer fits
