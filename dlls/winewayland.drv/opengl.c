/*
 * Wayland OpenGL functions
 *
 * Copyright 2020 Alexandros Frantzis for Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ntstatus.h"
#include "waylanddrv.h"
#include "wine/debug.h"

#ifdef HAVE_LIBWAYLAND_EGL

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

#include <wayland-egl.h>

#include "wine/opengl_driver.h"

static const struct egl_platform *egl;
static const struct opengl_funcs *funcs;
static const struct opengl_drawable_funcs wayland_drawable_funcs;
static BOOL (*p_egl_describe_pixel_format)( int format, struct wgl_pixel_format *pf );

/* How long a swap waits for the frame callback of the previous one. The compositor
 * is free to never send it for a surface that is not shown (hidden, minimized,
 * covered, on another desktop). Once a callback is missed the swaps go on at a
 * fixed pace, until it arrives. */
#define FRAME_CALLBACK_TIMEOUT_MS 100
#define FRAME_MISSED_PACE_MS 16

struct wayland_gl_drawable
{
    struct opengl_drawable base;
    struct wl_egl_window *wl_egl_window;
    struct wl_event_queue *wl_event_queue;   /* queue of the frame callbacks */
    struct wl_surface *wl_surface_wrapper;   /* client wl_surface, with events on wl_event_queue */
    struct wl_callback *frame_callback;      /* frame callback of an earlier swap, if still pending */
    BOOL frame_missed;                       /* the last swap gave up waiting for frame_callback */
    struct timespec last_swap;               /* time of the last swap */
};

static struct wayland_gl_drawable *impl_from_opengl_drawable(struct opengl_drawable *base)
{
    return CONTAINING_RECORD(base, struct wayland_gl_drawable, base);
}

static void wayland_drawable_destroy(struct opengl_drawable *base)
{
    struct wayland_gl_drawable *gl = impl_from_opengl_drawable(base);
    if (gl->frame_callback) wl_callback_destroy(gl->frame_callback);
    if (gl->wl_surface_wrapper) wl_proxy_wrapper_destroy(gl->wl_surface_wrapper);
    if (gl->wl_event_queue) wl_event_queue_destroy(gl->wl_event_queue);
    if (gl->wl_egl_window) wl_egl_window_destroy(gl->wl_egl_window);
}

static void frame_callback_handle_done(void *data, struct wl_callback *callback, uint32_t time)
{
    struct wayland_gl_drawable *gl = data;

    if (gl->frame_callback == callback) gl->frame_callback = NULL;
    wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_callback_listener =
{
    frame_callback_handle_done
};

static int timespec_diff_ms(const struct timespec *end, const struct timespec *start)
{
    return (end->tv_sec - start->tv_sec) * 1000 + (end->tv_nsec - start->tv_nsec) / 1000000;
}

/* Wait for the pending frame callback, but not for longer than FRAME_CALLBACK_TIMEOUT_MS,
 * or than the rest of FRAME_MISSED_PACE_MS since the last swap if that one gave up on it
 * already. A callback that didn't come stays pending, so there is never more than one. */
static void wayland_gl_drawable_wait_frame(struct wayland_gl_drawable *gl)
{
    struct wl_display *display = process_wayland.wl_display;
    struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLIN};
    int ret, remaining, timeout = FRAME_CALLBACK_TIMEOUT_MS;
    struct timespec start, now;

    clock_gettime(CLOCK_MONOTONIC, &start);
    if (gl->frame_missed) timeout = FRAME_MISSED_PACE_MS - timespec_diff_ms(&start, &gl->last_swap);

    while (gl->frame_callback)
    {
        if (wl_display_prepare_read_queue(display, gl->wl_event_queue) == -1)
        {
            if (wl_display_dispatch_queue_pending(display, gl->wl_event_queue) == -1) break;
            continue;
        }
        wl_display_flush(display);

        clock_gettime(CLOCK_MONOTONIC, &now);
        remaining = timeout - timespec_diff_ms(&now, &start);
        if (remaining <= 0) ret = 0;
        else while ((ret = poll(&pfd, 1, remaining)) == -1 && errno == EINTR) /* nothing */;

        if (ret <= 0)
        {
            wl_display_cancel_read(display);
            break;
        }
        if (wl_display_read_events(display) == -1) break;
        if (wl_display_dispatch_queue_pending(display, gl->wl_event_queue) == -1) break;
    }

    if (gl->frame_callback && !gl->frame_missed)
        TRACE("drawable %s, no frame callback within %d ms\n", debugstr_opengl_drawable(&gl->base), timeout);
    gl->frame_missed = gl->frame_callback != NULL;
}

static EGLConfig egl_config_for_format(int format)
{
    return egl->configs[(format - 1) % egl->config_count];
}

static BOOL wayland_opengl_surface_create(struct client_surface *client, int format, struct opengl_drawable **drawable)
{
    struct wayland_client_surface *surface = impl_from_client_surface(client);
    EGLConfig config = egl_config_for_format(format);
    EGLint attribs[5], *attrib = attribs;
    struct wayland_gl_drawable *gl;
    struct wgl_pixel_format desc;
    HWND hwnd = client->hwnd;
    SIZE size;

    TRACE("client=%s format=%d\n", debugstr_client_surface(client), format);

    p_egl_describe_pixel_format( format, &desc );

    if (!egl->extensions[EGL_EXT_present_opaque])
        WARN("Missing EGL_EXT_present_opaque extension\n");
    else
    {
        *attrib++ = EGL_PRESENT_OPAQUE_EXT;
        *attrib++ = EGL_TRUE;
    }
    if (desc.framebuffer_srgb_capable)
    {
        *attrib++ = EGL_GL_COLORSPACE;
        *attrib++ = EGL_GL_COLORSPACE_SRGB;
    }
    *attrib++ = EGL_NONE;

    if (!(gl = opengl_drawable_create(&wayland_drawable_funcs, format, client, NULL))) return FALSE;
    size = client->raw ? gl->base.monitor_size : gl->base.virtual_size;

    opengl_drawable_map_buffer(&gl->base, GL_FRONT_LEFT, GL_BACK_LEFT);
    opengl_drawable_map_buffer(&gl->base, GL_FRONT, GL_BACK);
    opengl_drawable_map_buffer(&gl->base, GL_FRONT_AND_BACK, GL_BACK);
    if (gl->base.stereo) opengl_drawable_map_buffer(&gl->base, GL_FRONT_RIGHT, GL_BACK_RIGHT);

    if (!(gl->wl_event_queue = wl_display_create_queue(process_wayland.wl_display))) goto err;
    if (!(gl->wl_surface_wrapper = wl_proxy_create_wrapper(surface->wl_surface))) goto err;
    wl_proxy_set_queue((struct wl_proxy *)gl->wl_surface_wrapper, gl->wl_event_queue);
    if (!(gl->wl_egl_window = wl_egl_window_create(surface->wl_surface, size.cx, size.cy))) goto err;
    if (!(gl->base.surface = funcs->p_eglCreateWindowSurface(egl->display, config, gl->wl_egl_window, attribs))) goto err;
    set_client_surface(hwnd, surface);

    TRACE("Created drawable %s with egl_surface %p\n", debugstr_opengl_drawable(&gl->base), gl->base.surface);

    *drawable = &gl->base;
    return TRUE;

err:
    opengl_drawable_release(&gl->base);
    return FALSE;
}

static void wayland_init_egl_platform(struct egl_platform *platform)
{
    platform->type = EGL_PLATFORM_WAYLAND_KHR;
    platform->native_display = process_wayland.wl_display;
    platform->force_pbuffer_formats = TRUE;
    egl = platform;
}

static void wayland_drawable_flush(struct opengl_drawable *base, UINT flags)
{
    struct wayland_gl_drawable *gl = impl_from_opengl_drawable(base);
    SIZE size = gl->base.client && gl->base.client->raw ? gl->base.monitor_size : gl->base.virtual_size;

    TRACE("drawable %s, flags %#x\n", debugstr_opengl_drawable(base), flags);

    /* With an interval other than zero eglSwapBuffers waits for the frame callback of
     * the previous swap, without a timeout (NVIDIA egl-wayland), and doesn't return for
     * a surface that is not shown anymore. Throttle in wayland_drawable_swap instead. */
    if (flags & GL_FLUSH_INTERVAL) funcs->p_eglSwapInterval(egl->display, 0);

    /* Since context_flush is called from operations that may latch the native size,
     * perform any pending resizes before calling them. */
    if (flags & GL_FLUSH_UPDATED) wl_egl_window_resize(gl->wl_egl_window, size.cx, size.cy, 0, 0);
}

static BOOL wayland_drawable_swap(struct opengl_drawable *base)
{
    struct wayland_gl_drawable *gl = impl_from_opengl_drawable(base);

    client_surface_present(base->client);

    if (base->interval)
    {
        wayland_gl_drawable_wait_frame(gl);
        clock_gettime(CLOCK_MONOTONIC, &gl->last_swap);
        /* requested here, committed by eglSwapBuffers together with the new buffer */
        if (!gl->frame_callback && (gl->frame_callback = wl_surface_frame(gl->wl_surface_wrapper)))
            wl_callback_add_listener(gl->frame_callback, &frame_callback_listener, gl);
    }

    funcs->p_eglSwapBuffers(egl->display, gl->base.surface);

    return TRUE;
}

struct wayland_pbuffer
{
    struct opengl_drawable base;
    struct wl_surface *surface;
    struct wl_egl_window *window;
};

static struct wayland_pbuffer *pbuffer_from_opengl_drawable(struct opengl_drawable *base)
{
    return CONTAINING_RECORD(base, struct wayland_pbuffer, base);
}

static void wayland_pbuffer_destroy(struct opengl_drawable *base)
{
    struct wayland_pbuffer *gl = pbuffer_from_opengl_drawable(base);

    TRACE("%s\n", debugstr_opengl_drawable(base));

    if (gl->window)
        wl_egl_window_destroy(gl->window);
    if (gl->surface)
        wl_surface_destroy(gl->surface);
}

static const struct opengl_drawable_funcs wayland_pbuffer_funcs =
{
    .size = sizeof(struct wayland_pbuffer),
    .destroy = wayland_pbuffer_destroy,
};

static BOOL wayland_pbuffer_create(HDC hdc, int format, SIZE size, BOOL largest, GLenum texture_format, GLenum texture_target,
                                   GLint max_level, struct opengl_drawable **drawable)
{
    EGLConfig config = egl_config_for_format(format);
    struct wayland_pbuffer *gl;

    TRACE("hdc %p, format %d, size %s, largest %u, texture_format %#x, texture_target %#x, max_level %#x, drawable %p\n",
          hdc, format, debugstr_size(&size), largest, texture_format, texture_target, max_level, drawable);

    if (!(gl = opengl_drawable_create(&wayland_pbuffer_funcs, format, NULL, &size))) return FALSE;
    /* Wayland EGL doesn't support pixmap or pbuffer, create a dummy window surface to act as the target render surface. */
    if (!(gl->surface = wl_compositor_create_surface(process_wayland.wl_compositor))) goto err;
    if (!(gl->window = wl_egl_window_create(gl->surface, size.cx, size.cy))) goto err;
    if (!(gl->base.surface = funcs->p_eglCreateWindowSurface(egl->display, config, gl->window, NULL))) goto err;

    TRACE("Created pbuffer %s with egl_surface %p\n", debugstr_opengl_drawable(&gl->base), gl->base.surface);
    *drawable = &gl->base;
    return TRUE;

err:
    opengl_drawable_release(&gl->base);
    return FALSE;
}

static struct opengl_driver_funcs wayland_driver_funcs =
{
    .p_init_egl_platform = wayland_init_egl_platform,
    .p_surface_create = wayland_opengl_surface_create,
    .p_pbuffer_create = wayland_pbuffer_create,
};

static const struct opengl_drawable_funcs wayland_drawable_funcs =
{
    .size = sizeof(struct wayland_gl_drawable),
    .destroy = wayland_drawable_destroy,
    .flush = wayland_drawable_flush,
    .swap = wayland_drawable_swap,
};

/**********************************************************************
 *           WAYLAND_OpenGLInit
 */
UINT WAYLAND_OpenGLInit(UINT version, const struct opengl_funcs *opengl_funcs, const struct opengl_driver_funcs **driver_funcs)
{
    if (version != WINE_OPENGL_DRIVER_VERSION)
    {
        ERR("Version mismatch, opengl32 wants %u but driver has %u\n",
            version, WINE_OPENGL_DRIVER_VERSION);
        return STATUS_INVALID_PARAMETER;
    }

    if (!opengl_funcs->egl_handle) return STATUS_NOT_SUPPORTED;
    funcs = opengl_funcs;

    wayland_driver_funcs.p_get_proc_address = (*driver_funcs)->p_get_proc_address;
    wayland_driver_funcs.p_init_pixel_formats = (*driver_funcs)->p_init_pixel_formats;
    wayland_driver_funcs.p_describe_pixel_format = (*driver_funcs)->p_describe_pixel_format;
    wayland_driver_funcs.p_init_extensions = (*driver_funcs)->p_init_extensions;
    wayland_driver_funcs.p_context_create = (*driver_funcs)->p_context_create;
    wayland_driver_funcs.p_context_destroy = (*driver_funcs)->p_context_destroy;
    wayland_driver_funcs.p_context_activate = (*driver_funcs)->p_context_activate;
    p_egl_describe_pixel_format = (*driver_funcs)->p_describe_pixel_format;

    *driver_funcs = &wayland_driver_funcs;
    return STATUS_SUCCESS;
}

#else /* No GL */

UINT WAYLAND_OpenGLInit(UINT version, const struct opengl_funcs *opengl_funcs, const struct opengl_driver_funcs **driver_funcs)
{
    return STATUS_NOT_IMPLEMENTED;
}

#endif
