/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
/*
  ALTERED SOURCE, not part of upstream SDL: a "mali" video driver for SDL3 on the TrimUI Smart Pro / Brick Pro
  (Allwinner A133P, PowerVR GE8300), ported to SDL3 from the SDL2 "mali-fbdev" driver in TrimUI's
  SDL2-2.26.1.GE8300 (itself SDL's mali-fbdev driver, patched for the GE8300). The PowerVR fbdev EGL ("null window
  system") takes EGL_DEFAULT_DISPLAY and a NULL native window: the surface is the whole screen (/dev/fb0). Input
  comes from evdev, as in the SDL2 driver. Added to an SDL3 source tree by apply.sh beside it.
*/
#include "SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_MALI

#include "../SDL_sysvideo.h"
#include "../SDL_egl_c.h"
#include "../../events/SDL_events_c.h"
#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_mouse_c.h"

#ifdef SDL_INPUT_LINUXEV
#include "../../core/linux/SDL_evdev.h"
#endif

#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct SDL_WindowData
{
    EGLSurface egl_surface;
};

static bool MALI_GLES_LoadLibrary(SDL_VideoDevice *_this, const char *path)
{
    return SDL_EGL_LoadLibrary(_this, path, EGL_DEFAULT_DISPLAY, 0);
}
SDL_EGL_CreateContext_impl(MALI)
SDL_EGL_SwapWindow_impl(MALI)
SDL_EGL_MakeCurrent_impl(MALI)

static bool MALI_VideoInit(SDL_VideoDevice *_this)
{
    struct fb_var_screeninfo vinfo;
    SDL_VideoDisplay display;
    SDL_DisplayMode mode;
    int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC, 0);
    if (fd < 0) {
        return SDL_SetError("mali: can't open /dev/fb0");
    }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        close(fd);
        return SDL_SetError("mali: can't read the framebuffer's size");
    }
    close(fd);

    SDL_zero(mode);
    mode.w = (int)vinfo.xres;
    mode.h = (int)vinfo.yres;
    mode.refresh_rate = 60.0f;
    mode.format = SDL_PIXELFORMAT_XBGR8888;

    SDL_zero(display);
    display.name = (char *)"PowerVR fbdev";
    display.desktop_mode = mode;
    if (SDL_AddVideoDisplay(&display, false) == 0) {
        return false;
    }
#ifdef SDL_INPUT_LINUXEV
    if (!SDL_EVDEV_Init()) {
        return false;
    }
#endif
    return true;
}

static void MALI_VideoQuit(SDL_VideoDevice *_this)
{
#ifdef SDL_INPUT_LINUXEV
    SDL_EVDEV_Quit();
#endif
}

static bool MALI_CreateWindow(SDL_VideoDevice *_this, SDL_Window *window, SDL_PropertiesID create_props)
{
    SDL_VideoDisplay *display = SDL_GetVideoDisplay(SDL_GetPrimaryDisplay());
    SDL_WindowData *data = (SDL_WindowData *)SDL_calloc(1, sizeof(SDL_WindowData));
    if (!data) {
        return false;
    }
    window->internal = data;

    // one window, the whole screen
    window->x = 0;
    window->y = 0;
    window->w = window->windowed.w = window->floating.w = display->desktop_mode.w;
    window->h = window->windowed.h = window->floating.h = display->desktop_mode.h;
    window->flags |= SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN;

    if (!_this->egl_data && !SDL_GL_LoadLibrary(NULL)) {
        return false;
    }
    data->egl_surface = SDL_EGL_CreateSurface(_this, window, (NativeWindowType)0);
    if (data->egl_surface == EGL_NO_SURFACE) {
        return SDL_SetError("mali: can't create the EGL surface");
    }
    SDL_SetMouseFocus(window);
    SDL_SetKeyboardFocus(window);
    return true;
}

static void MALI_DestroyWindow(SDL_VideoDevice *_this, SDL_Window *window)
{
    SDL_WindowData *data = window->internal;
    if (data) {
        if (data->egl_surface != EGL_NO_SURFACE) {
            SDL_EGL_DestroySurface(_this, data->egl_surface);
        }
        SDL_free(data);
    }
    window->internal = NULL;
}

static SDL_FullscreenResult MALI_SetWindowFullscreen(SDL_VideoDevice *_this, SDL_Window *window, SDL_VideoDisplay *display, SDL_FullscreenOp fullscreen)
{
    return SDL_FULLSCREEN_SUCCEEDED;   // always the whole screen
}

static void MALI_PumpEvents(SDL_VideoDevice *_this)
{
#ifdef SDL_INPUT_LINUXEV
    SDL_EVDEV_Poll();
#endif
}

static void MALI_Destroy(SDL_VideoDevice *device)
{
    SDL_free(device);
}

static SDL_VideoDevice *MALI_Create(void)
{
    SDL_VideoDevice *device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        return NULL;
    }
    device->free = MALI_Destroy;
    device->VideoInit = MALI_VideoInit;
    device->VideoQuit = MALI_VideoQuit;
    device->CreateSDLWindow = MALI_CreateWindow;
    device->DestroyWindow = MALI_DestroyWindow;
    device->SetWindowFullscreen = MALI_SetWindowFullscreen;
    device->PumpEvents = MALI_PumpEvents;

    device->GL_LoadLibrary = MALI_GLES_LoadLibrary;
    device->GL_GetProcAddress = SDL_EGL_GetProcAddressInternal;
    device->GL_UnloadLibrary = SDL_EGL_UnloadLibrary;
    device->GL_CreateContext = MALI_GLES_CreateContext;
    device->GL_MakeCurrent = MALI_GLES_MakeCurrent;
    device->GL_SetSwapInterval = SDL_EGL_SetSwapInterval;
    device->GL_GetSwapInterval = SDL_EGL_GetSwapInterval;
    device->GL_SwapWindow = MALI_GLES_SwapWindow;
    device->GL_DestroyContext = SDL_EGL_DestroyContext;

    device->device_caps = VIDEO_DEVICE_CAPS_SENDS_FULLSCREEN_DIMENSIONS | VIDEO_DEVICE_CAPS_FULLSCREEN_ONLY;
    return device;
}

VideoBootStrap MALI_bootstrap = {
    "mali",
    "PowerVR GE8300 fbdev EGL (TrimUI)",
    MALI_Create,
    NULL, // no ShowMessageBox
    false
};

#endif // SDL_VIDEO_DRIVER_MALI
