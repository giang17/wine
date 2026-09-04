/*
 * X11DRV desktop window handling
 *
 * Copyright 2001 Alexandre Julliard
 * Copyright 2020 Zhiyi Zhang for CodeWeavers
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
#include <X11/cursorfont.h>
#include <X11/Xlib.h>

#include "x11drv.h"

/* avoid conflict with field names in included win32 headers */
#undef Status
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(x11drv);

static RECT host_primary_rect;

#define _NET_WM_STATE_REMOVE 0
#define _NET_WM_STATE_ADD 1

/* Return TRUE if Wine is currently in virtual desktop mode */
BOOL is_virtual_desktop(void)
{
    return root_window != DefaultRootWindow( gdi_display );
}

/***********************************************************************
 *		X11DRV_init_desktop
 *
 * Setup the desktop when not using the root window.
 */
void X11DRV_init_desktop( Window win )
{
    host_primary_rect = get_host_primary_monitor_rect();
    root_window = win;
    managed_mode = FALSE;  /* no managed windows in desktop mode */
}

/***********************************************************************
 *           X11DRV_CreateDesktop
 *
 * Create the X11 desktop window for the desktop mode.
 */
BOOL X11DRV_CreateDesktop( const WCHAR *name, UINT width, UINT height )
{
    XSetWindowAttributes win_attr;
    Window win;
    Display *display = thread_init_display();

    TRACE( "%s %ux%u\n", debugstr_w(name), width, height );

    /* The VD root stays RGB24 -- an ARGB32 root breaks Wine's XRender pipeline
     * (RenderCreatePicture BadMatch, window mapping stalls; see issue 64).  The
     * per-pixel-alpha mini-compositor (vd_compositor.c) instead composites the
     * ARGB32 children onto a separate ARGB32 overlay window (a true top-level
     * that the host compositor blends with per-pixel alpha).  SubstructureNotify
     * Mask lets the compositor receive Map/Configure/Destroy of the override-
     * redirect top-levels that become direct children of the VD root. */
    win_attr.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | EnterWindowMask |
                          PointerMotionMask | ButtonPressMask | ButtonReleaseMask | FocusChangeMask |
                          StructureNotifyMask | PropertyChangeMask | SubstructureNotifyMask;
    win_attr.cursor = XCreateFontCursor( display, XC_top_left_arrow );

    if (default_visual.visual != DefaultVisual( display, DefaultScreen(display) ))
        win_attr.colormap = XCreateColormap( display, DefaultRootWindow(display),
                                             default_visual.visual, AllocNone );
    else
        win_attr.colormap = None;

    win = XCreateWindow( display, DefaultRootWindow(display),
                         0, 0, width, height, 0, default_visual.depth, InputOutput,
                         default_visual.visual, CWEventMask | CWCursor | CWColormap, &win_attr );
    if (!win) return FALSE;

    x11drv_xinput2_enable( display, win );
    XFlush( display );

    X11DRV_init_desktop( win );
    /* The VD root is created on the thread display, but the compositor renders
     * via the global gdi_display; sync so the window is server-visible before
     * the compositor's XGetWindowAttributes runs on the other connection.
     * The thread display is handed over as well: it is the connection that
     * selected the root's SubstructureNotify and the one whose event queue
     * X11DRV_ProcessEvents drains, so the XDamage objects have to live on it
     * for their notifications to ever be read. */
    XSync( display, False );
    vd_compositor_init( display, win );
    return TRUE;
}

BOOL is_desktop_fullscreen(void)
{
    RECT primary_rect = NtUserGetPrimaryMonitorRect();
    return (primary_rect.right - primary_rect.left == host_primary_rect.right - host_primary_rect.left &&
            primary_rect.bottom - primary_rect.top == host_primary_rect.bottom - host_primary_rect.top);
}
