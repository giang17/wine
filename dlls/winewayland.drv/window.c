/*
 * Wayland window handling
 *
 * Copyright 2020 Alexandros Frantzis for Collabora Ltd
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
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS

#include "waylanddrv.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);


static int wayland_win_data_cmp_rb(const void *key,
                                   const struct rb_entry *entry)
{
    HWND key_hwnd = (HWND)key; /* cast to work around const */
    const struct wayland_win_data *entry_win_data =
        RB_ENTRY_VALUE(entry, const struct wayland_win_data, entry);

    if (key_hwnd < entry_win_data->hwnd) return -1;
    if (key_hwnd > entry_win_data->hwnd) return 1;
    return 0;
}

static pthread_mutex_t win_data_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct rb_tree win_data_rb = { wayland_win_data_cmp_rb };

/***********************************************************************
 *           wayland_win_data_create
 *
 * Create a data window structure for an existing window.
 */
static struct wayland_win_data *wayland_win_data_create(HWND hwnd, const struct window_rects *rects)
{
    struct wayland_win_data *data;
    struct rb_entry *rb_entry;
    HWND parent;

    /* Don't create win data for desktop or HWND_MESSAGE windows. */
    if (!(parent = NtUserGetAncestor(hwnd, GA_PARENT))) return NULL;
    if (parent != NtUserGetDesktopWindow() && !NtUserGetAncestor(parent, GA_PARENT))
        return NULL;

    if (!(data = calloc(1, sizeof(*data)))) return NULL;

    data->hwnd = hwnd;
    data->rects = *rects;

    pthread_mutex_lock(&win_data_mutex);

    /* Check that another thread hasn't already created the wayland_win_data. */
    if ((rb_entry = rb_get(&win_data_rb, hwnd)))
    {
        free(data);
        return RB_ENTRY_VALUE(rb_entry, struct wayland_win_data, entry);
    }

    rb_put(&win_data_rb, hwnd, &data->entry);

    TRACE("hwnd=%p\n", data->hwnd);

    return data;
}

/***********************************************************************
 *           wayland_win_data_clear_xdg_parent
 *
 * The toplevel of the window is going away: the compositor forgets its parent
 * and gives its children to that parent. Called with the win_data lock held.
 */
static void wayland_win_data_clear_xdg_parent(struct wayland_win_data *data)
{
    struct wayland_win_data *other;

    RB_FOR_EACH_ENTRY(other, &win_data_rb, struct wayland_win_data, entry)
        if (other->xdg_parent == data->hwnd) other->xdg_parent = data->xdg_parent;
    data->xdg_parent = 0;
}

/***********************************************************************
 *           wayland_win_data_destroy
 */
static void wayland_win_data_destroy(struct wayland_win_data *data)
{
    TRACE("hwnd=%p\n", data->hwnd);

    wayland_win_data_clear_xdg_parent(data);
    rb_remove(&win_data_rb, &data->entry);

    pthread_mutex_unlock(&win_data_mutex);

    if (data->wayland_surface) wayland_surface_destroy(data->wayland_surface);
    if (data->window_contents) wayland_shm_buffer_unref(data->window_contents);
    free(data);
}

/***********************************************************************
 *           wayland_win_data_get_nolock
 *
 * Return the data structure associated with a window. This function does
 * not lock the win_data_mutex, so it must be externally synchronized.
 */
struct wayland_win_data *wayland_win_data_get_nolock(HWND hwnd)
{
    struct rb_entry *rb_entry;

    if ((rb_entry = rb_get(&win_data_rb, hwnd)))
        return RB_ENTRY_VALUE(rb_entry, struct wayland_win_data, entry);

    return NULL;
}

/***********************************************************************
 *           wayland_win_data_get
 *
 * Lock and return the data structure associated with a window.
 */
struct wayland_win_data *wayland_win_data_get(HWND hwnd)
{
    struct wayland_win_data *data;

    pthread_mutex_lock(&win_data_mutex);
    if ((data = wayland_win_data_get_nolock(hwnd))) return data;
    pthread_mutex_unlock(&win_data_mutex);

    return NULL;
}

/***********************************************************************
 *           wayland_win_data_release
 *
 * Release the data returned by wayland_win_data_get.
 */
void wayland_win_data_release(struct wayland_win_data *data)
{
    assert(data);
    pthread_mutex_unlock(&win_data_mutex);
}

static void wayland_win_data_get_config(struct wayland_win_data *data,
                                        struct wayland_window_config *conf)
{
    enum wayland_surface_config_state window_state = 0;
    DWORD style;

    conf->rect = data->rects.window;
    conf->client_rect = data->rects.client;
    style = NtUserGetWindowLongW(data->hwnd, GWL_STYLE);

    TRACE("window=%s style=%#x\n", wine_dbgstr_rect(&conf->rect), style);

    conf->minimized = !!(style & WS_MINIMIZE);

    /* The fullscreen state is implied by the window position and style. */
    if (data->is_fullscreen)
    {
        if ((style & WS_MAXIMIZE) && (style & WS_CAPTION) == WS_CAPTION)
            window_state |= WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED;
        else if (!(style & WS_MINIMIZE))
            window_state |= WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN;
    }
    else if (style & WS_MAXIMIZE)
    {
        window_state |= WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED;
    }

    conf->state = window_state;
    conf->visible = (style & WS_VISIBLE) == WS_VISIBLE;
    conf->managed = data->managed;
}

static void reapply_cursor_clipping(void)
{
    RECT rect;
    UINT context = NtUserSetThreadDpiAwarenessContext(NTUSER_DPI_PER_MONITOR_AWARE);
    if (NtUserGetClipCursor(&rect )) NtUserClipCursor(&rect);
    NtUserSetThreadDpiAwarenessContext(context);
}

static BOOL wayland_win_data_create_wayland_surface(struct wayland_win_data *data, struct wayland_surface *toplevel_surface)
{
    struct wayland_client_surface *client = data->client_surface;
    struct wayland_surface *surface;
    enum wayland_surface_role role;
    BOOL visible;
    DWORD exstyle = NtUserGetWindowLongW(data->hwnd, GWL_EXSTYLE);
    struct wl_region *input_region;

    TRACE("hwnd=%p\n", data->hwnd);

    visible = ((NtUserGetWindowLongW(data->hwnd, GWL_STYLE) & WS_VISIBLE) == WS_VISIBLE) &&
               (!(exstyle & WS_EX_LAYERED) || data->layered_attribs_set);

    if (!visible) role = WAYLAND_SURFACE_ROLE_NONE;
    else if (toplevel_surface) role = WAYLAND_SURFACE_ROLE_SUBSURFACE;
    else role = WAYLAND_SURFACE_ROLE_TOPLEVEL;

    /* we can temporarily clear the role of a surface but cannot assign a different one after it's set */
    if ((surface = data->wayland_surface) && role && surface->role && surface->role != role)
    {
        if (client) wayland_client_surface_attach(client, NULL);
        wayland_surface_destroy(data->wayland_surface);
        data->wayland_surface = NULL;
    }

    if (!(surface = data->wayland_surface) && !(surface = wayland_surface_create(data->hwnd))) return FALSE;

    /* Pass through mouse events for layered, transparent windows, to match
     * Windows behavior. */
    input_region = ((exstyle & WS_EX_TRANSPARENT) && (exstyle & WS_EX_LAYERED)) ?
                   wl_compositor_create_region(process_wayland.wl_compositor) :
                   NULL;
    wl_surface_set_input_region(surface->wl_surface, input_region);
    if (input_region) wl_region_destroy(input_region);

    /* If the window is a visible toplevel make it a wayland
     * xdg_toplevel. Otherwise keep it role-less to avoid polluting the
     * compositor with empty xdg_toplevels. */
    switch (role)
    {
    case WAYLAND_SURFACE_ROLE_NONE:
        wayland_surface_clear_role(surface);
        break;
    case WAYLAND_SURFACE_ROLE_TOPLEVEL:
        wayland_surface_make_toplevel(surface);
        break;
    case WAYLAND_SURFACE_ROLE_SUBSURFACE:
        wayland_surface_make_subsurface(surface, toplevel_surface);
        break;
    }

    if (visible && client) wayland_client_surface_attach(client, data->hwnd);
    wayland_win_data_get_config(data, &surface->window);

    /* Size/position changes affect the effective pointer constraint, so update
     * it as needed. */
    if (data->hwnd == NtUserGetForegroundWindow()) reapply_cursor_clipping();

    TRACE("hwnd=%p surface=%p=>%p\n", data->hwnd, data->wayland_surface, surface);
    data->wayland_surface = surface;
    return TRUE;
}

static void wayland_surface_update_state_toplevel(struct wayland_surface *surface)
{
    BOOL processing_config = surface->processing.serial &&
                             !surface->processing.processed;

    TRACE("hwnd=%p window_state=%#x %s->state=%#x\n",
          surface->hwnd, surface->window.state,
          processing_config ? "processing" : "current",
          processing_config ? surface->processing.state : surface->current.state);

    /* If we are not processing a compositor requested config, use the
     * window state to determine and update the Wayland state. */
    if (!processing_config)
    {
         /* First do all state unsettings, before setting new state. Some
          * Wayland compositors misbehave if the order is reversed. */
        if (!(surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) &&
            (surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) &&
            !surface->window.minimized)
        {
            xdg_toplevel_unset_maximized(surface->xdg_toplevel);
        }
        if (!(surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN) &&
            (surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN) &&
            !surface->window.minimized)
        {
            xdg_toplevel_unset_fullscreen(surface->xdg_toplevel);
        }

        if ((surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) &&
           !(surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED))
        {
            xdg_toplevel_set_maximized(surface->xdg_toplevel);
        }
        if ((surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN) &&
           !(surface->current.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN))
        {
            xdg_toplevel_set_fullscreen(surface->xdg_toplevel, NULL);
        }
        if (surface->window.minimized)
        {
            xdg_toplevel_set_minimized(surface->xdg_toplevel);
        }
    }
    else
    {
        surface->processing.processed = TRUE;
    }
}

static void wayland_win_data_update_wayland_state(struct wayland_win_data *data)
{
    struct wayland_surface *surface = data->wayland_surface;

    switch (surface->role)
    {
    case WAYLAND_SURFACE_ROLE_NONE:
        break;
    case WAYLAND_SURFACE_ROLE_TOPLEVEL:
        if (!surface->xdg_surface) break; /* surface role has been cleared */
        wayland_surface_update_state_toplevel(surface);
        break;
    case WAYLAND_SURFACE_ROLE_SUBSURFACE:
        TRACE("hwnd=%p subsurface parent=%p\n", surface->hwnd, surface->toplevel_hwnd);
        /* Although subsurfaces don't have a dedicated surface config mechanism,
         * we use the config fields to mark them as updated. */
        surface->processing.serial = 1;
        surface->processing.processed = TRUE;
        break;
    }

    wl_display_flush(process_wayland.wl_display);
}

static BOOL is_managed(HWND hwnd)
{
    struct wayland_win_data *data = wayland_win_data_get(hwnd);
    BOOL ret = data && data->managed;
    if (data) wayland_win_data_release(data);
    return ret;
}

static HWND *build_hwnd_list(void)
{
    NTSTATUS status;
    HWND *list;
    ULONG count = 128;

    for (;;)
    {
        if (!(list = malloc(count * sizeof(*list)))) return NULL;
        status = NtUserBuildHwndList(0, 0, 0, 0, 0, count, list, &count);
        if (!status) return list;
        free(list);
        if (status != STATUS_BUFFER_TOO_SMALL) return NULL;
    }
}

static BOOL has_owned_popups(HWND hwnd)
{
    HWND *list;
    UINT i;
    BOOL ret = FALSE;

    if (!(list = build_hwnd_list())) return FALSE;

    for (i = 0; list[i] != HWND_BOTTOM; i++)
    {
        if (list[i] == hwnd) break;  /* popups are always above owner */
        if (NtUserGetWindowRelative(list[i], GW_OWNER) != hwnd) continue;
        if ((ret = is_managed(list[i]))) break;
    }

    free(list);
    return ret;
}

#define MAX_OWNERS 8

/* The owner of a window and the owners of that one in turn, nearest first.
 * Must be called without the win_data lock. */
static UINT get_window_owners(HWND hwnd, HWND owners[MAX_OWNERS])
{
    UINT count = 0;

    while (count < MAX_OWNERS && (hwnd = NtUserGetWindowRelative(hwnd, GW_OWNER))) owners[count++] = hwnd;
    return count;
}

static struct wayland_surface *get_mapped_toplevel(struct wayland_win_data *data)
{
    struct wayland_surface *surface;

    if (!data || !(surface = data->wayland_surface)) return NULL;
    if (!wayland_surface_is_toplevel(surface) || !surface->content_width) return NULL;
    return surface;
}

/***********************************************************************
 *           wayland_win_data_update_xdg_parent
 *
 * Tell the compositor which toplevel a managed window belongs to, so that it
 * keeps the window above its owner: the nearest owner that is mapped, as only
 * a mapped toplevel can have children. Called with the win_data lock held, with
 * owners read before taking it. Returns TRUE if the parents of other windows
 * have to be looked at again.
 */
static BOOL wayland_win_data_update_xdg_parent(struct wayland_win_data *data, const HWND *owners, UINT count)
{
    struct wayland_surface *surface = data->wayland_surface, *parent_surface = NULL;
    struct wayland_win_data *parent_data = NULL, *ancestor;
    HWND hwnd, parent = 0;
    BOOL stale = FALSE;
    UINT i;

    if (!surface || !wayland_surface_is_toplevel(surface)) return FALSE;

    for (i = 0; i < count && !parent_surface; i++)
    {
        parent_data = wayland_win_data_get_nolock(owners[i]);
        if ((parent_surface = get_mapped_toplevel(parent_data))) parent = owners[i];
    }
    if (!parent && !data->xdg_parent) return FALSE;

    /* A parent that is a descendant of its child is a protocol error. The owner of
     * a window can change without the driver being told, so a window on the way up
     * may still have this one as its parent for the compositor. */
    for (hwnd = parent, i = 0; hwnd && (ancestor = wayland_win_data_get_nolock(hwnd)); hwnd = ancestor->xdg_parent)
    {
        if (ancestor->xdg_parent != data->hwnd && ++i < 64) continue;
        TRACE("hwnd=%p dropping parent %p of %p\n", data->hwnd, ancestor->xdg_parent, hwnd);
        if (ancestor->wayland_surface && wayland_surface_is_toplevel(ancestor->wayland_surface))
            xdg_toplevel_set_parent(ancestor->wayland_surface->xdg_toplevel, NULL);
        ancestor->xdg_parent = 0;
        stale = TRUE;
        break;
    }

    TRACE("hwnd=%p parent %p -> %p\n", data->hwnd, data->xdg_parent, parent);
    xdg_toplevel_set_parent(surface->xdg_toplevel, parent_surface ? parent_surface->xdg_toplevel : NULL);
    data->xdg_parent = parent;
    return stale;
}

/***********************************************************************
 *           wayland_update_xdg_parents
 *
 * Set the parents of all managed windows of the process again: a toplevel
 * was mapped and can have children now, or one went away.
 */
static void wayland_update_xdg_parents(void)
{
    HWND *list, (*owners)[MAX_OWNERS];
    struct wayland_win_data *data;
    UINT i, pass, count = 0, *owner_count;
    BOOL again = TRUE;
    DWORD pid;

    if (!(list = build_hwnd_list())) return;
    while (list[count] != HWND_BOTTOM) count++;
    owners = malloc(count * sizeof(*owners));
    owner_count = calloc(count, sizeof(*owner_count));

    if (owners && owner_count)
    {
        for (i = 0; i < count; i++)
        {
            if (!NtUserGetWindowThread(list[i], &pid) || pid != GetCurrentProcessId()) continue;
            owner_count[i] = get_window_owners(list[i], owners[i]);
        }

        pthread_mutex_lock(&win_data_mutex);
        for (pass = 0; again && pass < 2; pass++)
        {
            again = FALSE;
            for (i = 0; i < count; i++)
            {
                if (!(data = wayland_win_data_get_nolock(list[i])) || !data->managed) continue;
                again |= wayland_win_data_update_xdg_parent(data, owners[i], owner_count[i]);
            }
        }
        pthread_mutex_unlock(&win_data_mutex);
        wl_display_flush(process_wayland.wl_display);
    }

    free(owner_count);
    free(owners);
    free(list);
}

static inline HWND get_active_window(void)
{
    GUITHREADINFO info;
    info.cbSize = sizeof(info);
    return NtUserGetGUIThreadInfo(GetCurrentThreadId(), &info) ? info.hwndActive : 0;
}

/***********************************************************************
 *		is_window_managed
 *
 * Check if a given window should be managed
 */
static BOOL is_window_managed(HWND hwnd, UINT swp_flags, BOOL fullscreen)
{
    DWORD style, ex_style;

    /* child windows are not managed */
    style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    if ((style & (WS_CHILD|WS_POPUP)) == WS_CHILD) return FALSE;
    /* activated windows are managed */
    if (!(swp_flags & (SWP_NOACTIVATE|SWP_HIDEWINDOW))) return TRUE;
    if (hwnd == get_active_window()) return TRUE;
    /* windows with caption are managed */
    if ((style & WS_CAPTION) == WS_CAPTION) return TRUE;
    /* windows with thick frame are managed */
    if (style & WS_THICKFRAME) return TRUE;
    if (style & WS_POPUP)
    {
        /* popup with sysmenu == caption are managed */
        if (style & WS_SYSMENU) return TRUE;
        /* full-screen popup windows are managed */
        if (fullscreen) return TRUE;
    }
    /* application windows are managed */
    ex_style = NtUserGetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex_style & WS_EX_APPWINDOW) return TRUE;
    /* windows that own popups are managed */
    if (has_owned_popups(hwnd)) return TRUE;
    /* default: not managed */
    return FALSE;
}

/***********************************************************************
 *           WAYLAND_DestroyWindow
 */
void WAYLAND_DestroyWindow(HWND hwnd)
{
    struct wayland_win_data *data;

    TRACE("%p\n", hwnd);

    if (!(data = wayland_win_data_get(hwnd))) return;
    wayland_win_data_destroy(data);
}

/***********************************************************************
 *           WAYLAND_WindowPosChanging
 */
BOOL WAYLAND_WindowPosChanging(HWND hwnd, UINT swp_flags, BOOL shaped, const struct window_rects *rects)
{
    struct wayland_win_data *data = wayland_win_data_get(hwnd);

    TRACE("hwnd %p, swp_flags %04x, shaped %u, rects %s\n", hwnd, swp_flags, shaped, debugstr_window_rects(rects));

    if (!data && !(data = wayland_win_data_create(hwnd, rects))) return FALSE;

    wayland_win_data_release(data);

    return TRUE;
}

/***********************************************************************
 *           WAYLAND_WindowPosChanged
 */
void WAYLAND_WindowPosChanged(HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                              const struct window_rects *new_rects, struct window_surface *surface)
{
    HWND toplevel = NtUserGetAncestor(hwnd, GA_ROOT);
    struct wayland_surface *toplevel_surface;
    struct wayland_client_surface *client;
    struct wayland_win_data *data, *toplevel_data;
    BOOL managed, fullscreen = swp_flags & WINE_SWP_FULLSCREEN;
    BOOL had_toplevel, update_parents = FALSE;
    HWND owners[MAX_OWNERS];
    UINT owner_count = 0;

    TRACE("hwnd %p new_rects %s after %p flags %08x\n", hwnd, debugstr_window_rects(new_rects), insert_after, swp_flags);

    /* Get the managed state with win_data unlocked, as is_window_managed
     * may need to query win_data information about other HWNDs and thus
     * acquire the lock itself internally. */
    if (!(managed = is_window_managed(hwnd, swp_flags, fullscreen)) && surface) toplevel = owner_hint;
    if (managed) owner_count = get_window_owners(hwnd, owners);

    if (!(data = wayland_win_data_get(hwnd))) return;
    had_toplevel = data->wayland_surface && wayland_surface_is_toplevel(data->wayland_surface);
    toplevel_data = toplevel && toplevel != hwnd ? wayland_win_data_get_nolock(toplevel) : NULL;
    toplevel_surface = toplevel_data ? toplevel_data->wayland_surface : NULL;

    data->rects = *new_rects;
    data->is_fullscreen = fullscreen;
    data->managed = managed;

    if (!surface)
    {
        if ((client = data->client_surface))
        {
            if (toplevel && NtUserIsWindowVisible(hwnd))
                wayland_client_surface_attach(client, toplevel);
            else
                wayland_client_surface_attach(client, NULL);
        }

        if (data->wayland_surface)
        {
            wayland_surface_destroy(data->wayland_surface);
            data->wayland_surface = NULL;
        }
    }
    else if (wayland_win_data_create_wayland_surface(data, toplevel_surface))
    {
        wayland_win_data_update_wayland_state(data);
    }

    if (data->wayland_surface && wayland_surface_is_toplevel(data->wayland_surface))
    {
        /* a new toplevel has no parent yet */
        if (!had_toplevel) data->xdg_parent = 0;
        update_parents = wayland_win_data_update_xdg_parent(data, owners, owner_count);
        wl_display_flush(process_wayland.wl_display);
    }
    else if (had_toplevel)
    {
        /* the windows it owns go to the next owner that is mapped */
        wayland_win_data_clear_xdg_parent(data);
        update_parents = TRUE;
    }

    wayland_win_data_release(data);

    if (update_parents) NtUserPostMessage(hwnd, WM_WAYLAND_UPDATE_XDG_PARENTS, 0, 0);
}

static void wayland_configure_window(HWND hwnd)
{
    struct wayland_surface *surface;
    INT width, height, window_width, window_height;
    INT window_surf_width, window_surf_height;
    UINT flags = 0;
    uint32_t state;
    DWORD style;
    BOOL needs_enter_size_move = FALSE;
    BOOL needs_exit_size_move = FALSE;
    BOOL restoring_from_minimize = FALSE;
    struct wayland_win_data *data;
    RECT rect;

    if (!(data = wayland_win_data_get(hwnd))) return;
    if (!(surface = data->wayland_surface))
    {
        wayland_win_data_release(data);
        return;
    }

    if (!wayland_surface_is_toplevel(surface))
    {
        TRACE("missing xdg_toplevel, returning\n");
        wayland_win_data_release(data);
        return;
    }

    if (!surface->requested.serial)
    {
        TRACE("requested configure event already handled, returning\n");
        wayland_win_data_release(data);
        return;
    }

    surface->processing = surface->requested;
    memset(&surface->requested, 0, sizeof(surface->requested));

    state = surface->processing.state;
    /* Ignore size hints if we don't have a state that requires strict
     * size adherence, in order to avoid spurious resizes. */
    if (state)
    {
        width = surface->processing.width;
        height = surface->processing.height;
    }
    else
    {
        width = height = 0;
    }

    if ((state & WAYLAND_SURFACE_CONFIG_STATE_RESIZING) && !surface->resizing)
    {
        surface->resizing = TRUE;
        needs_enter_size_move = TRUE;
    }

    if (!(state & WAYLAND_SURFACE_CONFIG_STATE_RESIZING) && surface->resizing)
    {
        surface->resizing = FALSE;
        needs_exit_size_move = TRUE;
    }

    /* Transitions between normal/max/fullscreen may entail a frame change. */
    if ((state ^ surface->current.state) &
        (WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED |
         WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN))
    {
        flags |= SWP_FRAMECHANGED;
    }

    wayland_surface_coords_from_window(surface,
                                       surface->window.rect.right -
                                           surface->window.rect.left,
                                       surface->window.rect.bottom -
                                           surface->window.rect.top,
                                       &window_surf_width, &window_surf_height);

    /* If the window is already fullscreen and its size is compatible with what
     * the compositor is requesting, don't force a resize, since some applications
     * are very insistent on a particular fullscreen size (which may not match
     * the monitor size). */
    if ((surface->window.state & WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN) &&
        wayland_surface_config_is_compatible(&surface->processing,
                                             window_surf_width, window_surf_height,
                                             surface->window.state))
    {
        flags |= SWP_NOSIZE;
    }

    wayland_surface_coords_to_window(surface, width, height,
                                     &window_width, &window_height);

    /* Detect a restore from an application-initiated minimize: the last
     * requested config placed the window at the offscreen sentinel position
     * with WS_MINIMIZE, and the compositor is now sending a configure. Ack
     * the configure to avoid a protocol violation and send SC_RESTORE so
     * Win32 runs the full restore sequence (clearing WS_MINIMIZE, restoring
     * position/size, sending WM_SIZE, etc.), which triggers a new configure
     * cycle. */
    restoring_from_minimize = surface->window.rect.left <= -32000 &&
                              surface->window.rect.top  <= -32000 &&
                              surface->window.minimized;
    if (restoring_from_minimize)
    {
        TRACE("hwnd=%p restoring from minimize\n", hwnd);
        surface->current = surface->processing;
        memset(&surface->processing, 0, sizeof(surface->processing));
        xdg_surface_ack_configure(surface->xdg_surface,
                                  surface->current.serial);
        wayland_win_data_release(data);
        send_message(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
        return;
    }

    SetRect(&rect, 0, 0, window_width, window_height);
    OffsetRect(&rect, data->rects.window.left, data->rects.window.top);

    wayland_win_data_release(data);

    TRACE("processing=%dx%d,%#x\n", width, height, state);

    if (needs_enter_size_move) send_message(hwnd, WM_ENTERSIZEMOVE, 0, 0);
    if (needs_exit_size_move) send_message(hwnd, WM_EXITSIZEMOVE, 0, 0);

    flags |= SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOMOVE;
    if (window_width == 0 || window_height == 0) flags |= SWP_NOSIZE;

    style = NtUserGetWindowLongW(hwnd, GWL_STYLE);
    if (!(state & WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED) != !(style & WS_MAXIMIZE))
        NtUserSetWindowLong(hwnd, GWL_STYLE, style ^ WS_MAXIMIZE, FALSE);

    /* The Wayland maximized and fullscreen states are very strict about
     * surface size, so don't let the application override it. The tiled state
     * is not as strict, but it indicates a strong size preference, so try to
     * respect it. */
    if (state & (WAYLAND_SURFACE_CONFIG_STATE_MAXIMIZED |
                 WAYLAND_SURFACE_CONFIG_STATE_FULLSCREEN |
                 WAYLAND_SURFACE_CONFIG_STATE_TILED))
    {
        flags |= SWP_NOSENDCHANGING;
    }

    NtUserSetRawWindowPos(hwnd, rect, flags, FALSE);
}

/**********************************************************************
 *           WAYLAND_WindowMessage
 */
LRESULT WAYLAND_WindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_WAYLAND_INIT_DISPLAY_DEVICES:
        NtUserCallNoParam(NtUserCallNoParam_DisplayModeChanged);
        return 0;
    case WM_WAYLAND_CONFIGURE:
        wayland_configure_window(hwnd);
        return 0;
    case WM_WAYLAND_SET_FOREGROUND:
        NtUserSetForegroundWindowInternal(hwnd);
        return 0;
    case WM_WAYLAND_UPDATE_XDG_PARENTS:
        wayland_update_xdg_parents();
        return 0;
    default:
        FIXME("got window msg %x hwnd %p wp %lx lp %lx\n", msg, hwnd, (long)wp, lp);
        return 0;
    }
}

/**********************************************************************
 *           WAYLAND_DesktopWindowProc
 */
LRESULT WAYLAND_DesktopWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return NtUserMessageCall(hwnd, msg, wp, lp, 0, NtUserDefWindowProc, FALSE);
}

/*****************************************************************
 *		WAYLAND_SetLayeredWindowAttributes
 */
void WAYLAND_SetLayeredWindowAttributes(HWND hwnd, COLORREF key, BYTE alpha, DWORD flags)
{
    struct wayland_win_data *data;

    if (!(data = wayland_win_data_get(hwnd))) return;
    data->layered_attribs_set = TRUE;
    wayland_win_data_release(data);
}

static enum xdg_toplevel_resize_edge hittest_to_resize_edge(WPARAM hittest)
{
    switch (hittest)
    {
    case WMSZ_LEFT:        return XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
    case WMSZ_RIGHT:       return XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
    case WMSZ_TOP:         return XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    case WMSZ_TOPLEFT:     return XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
    case WMSZ_TOPRIGHT:    return XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
    case WMSZ_BOTTOM:      return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    case WMSZ_BOTTOMLEFT:  return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
    case WMSZ_BOTTOMRIGHT: return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
    default:               return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
    }
}

/*****************************************************************
 *		WAYLAND_SetWindowIcons
 */
void WAYLAND_SetWindowIcons(HWND hwnd, HICON icon, const ICONINFO *ii, HICON icon_small, const ICONINFO *ii_small)
{
    struct wayland_surface *surface;
    struct wayland_win_data *data;

    TRACE("hwnd=%p icon=%p ii=%p icon_small=%p ii_small=%p\n", hwnd, icon, ii, icon_small, ii_small);

    if (process_wayland.xdg_toplevel_icon_manager_v1)
    {
        if ((data = wayland_win_data_get(hwnd)))
        {
            if ((surface = data->wayland_surface))
            {
                wayland_surface_set_icon_buffer(surface, ICON_BIG, ii);
                if (icon_small) wayland_surface_set_icon_buffer(surface, ICON_SMALL, ii_small);
                if (wayland_surface_is_toplevel(surface))
                    wayland_surface_assign_icon(surface);
            }
            wayland_win_data_release(data);
        }
    }
}

/***********************************************************************
 *		WAYLAND_SetWindowStyle
 */
void WAYLAND_SetWindowStyle(HWND hwnd, INT offset, STYLESTRUCT *style)
{
    struct wayland_win_data *data;
    DWORD changed = style->styleNew ^ style->styleOld;

    if (hwnd == NtUserGetDesktopWindow()) return;
    if (!(data = wayland_win_data_get(hwnd))) return;

    /* Changing WS_EX_LAYERED resets attributes */
    if (offset == GWL_EXSTYLE && (changed & WS_EX_LAYERED))
        data->layered_attribs_set = FALSE;

    wayland_win_data_release(data);
}

/*****************************************************************
 *		WAYLAND_SetWindowText
 */
void WAYLAND_SetWindowText(HWND hwnd, LPCWSTR text)
{
    struct wayland_surface *surface;
    struct wayland_win_data *data;

    TRACE("hwnd=%p text=%s\n", hwnd, wine_dbgstr_w(text));

    if ((data = wayland_win_data_get(hwnd)))
    {
        if ((surface = data->wayland_surface) && wayland_surface_is_toplevel(surface))
            wayland_surface_set_title(surface, text);
        wayland_win_data_release(data);
    }
}

/***********************************************************************
 *          WAYLAND_SysCommand
 */
LRESULT WAYLAND_SysCommand(HWND hwnd, WPARAM wparam, LPARAM lparam, const POINT *pos)
{
    LRESULT ret = -1;
    WPARAM command = wparam & 0xfff0;
    uint32_t button_serial;
    struct wl_seat *wl_seat;
    struct wayland_surface *surface;
    struct wayland_win_data *data;

    TRACE("cmd=%lx hwnd=%p, %lx, %lx\n",
          (long)command, hwnd, (long)wparam, lparam);

    pthread_mutex_lock(&process_wayland.pointer.mutex);
    if (process_wayland.pointer.focused_hwnd == hwnd)
        button_serial = process_wayland.pointer.button_serial;
    else
        button_serial = 0;
    pthread_mutex_unlock(&process_wayland.pointer.mutex);

    if (command == SC_MOVE || command == SC_SIZE)
    {
        if ((data = wayland_win_data_get(hwnd)))
        {
            pthread_mutex_lock(&process_wayland.seat.mutex);
            wl_seat = process_wayland.seat.wl_seat;
            if (wl_seat && (surface = data->wayland_surface) &&
                wayland_surface_is_toplevel(surface) && button_serial)
            {
                if (command == SC_MOVE)
                {
                    xdg_toplevel_move(surface->xdg_toplevel, wl_seat, button_serial);
                }
                else if (command == SC_SIZE)
                {
                    xdg_toplevel_resize(surface->xdg_toplevel, wl_seat, button_serial,
                                        hittest_to_resize_edge(wparam & 0x0f));
                }
            }
            pthread_mutex_unlock(&process_wayland.seat.mutex);
            wayland_win_data_release(data);
            ret = 0;
        }
    }

    wl_display_flush(process_wayland.wl_display);
    return ret;
}

void set_client_surface(HWND hwnd, struct wayland_client_surface *new_client)
{
    HWND toplevel = NtUserGetAncestor(hwnd, GA_ROOT);
    struct wayland_client_surface *old_client;
    struct wayland_win_data *data;

    /* ownership is shared with the callers, the last caller to release
     * its reference will also destroy it and clear our pointer. */

    if (!(data = wayland_win_data_get(hwnd))) return;

    if (new_client != data->client_surface)
    {
        if ((old_client = data->client_surface))
            wayland_client_surface_attach(old_client, NULL);

        if ((data->client_surface = new_client))
        {
            if (toplevel && NtUserIsWindowVisible(hwnd))
                wayland_client_surface_attach(new_client, toplevel);
            else
                wayland_client_surface_attach(new_client, NULL);
        }
    }

    wayland_win_data_release(data);
}

BOOL set_window_surface_contents(HWND hwnd, struct wayland_shm_buffer *shm_buffer, HRGN damage_region)
{
    struct wayland_surface *wayland_surface;
    struct wayland_win_data *data;
    BOOL committed = FALSE, mapped;

    if (!(data = wayland_win_data_get(hwnd))) return FALSE;
    mapped = get_mapped_toplevel(data) != NULL;

    if ((wayland_surface = data->wayland_surface))
    {
        if (wayland_surface_reconfigure(wayland_surface))
        {
            wayland_surface_attach_shm(wayland_surface, shm_buffer, damage_region);
            wl_surface_commit(wayland_surface->wl_surface);
            committed = TRUE;
        }
        else
        {
            TRACE("Wayland surface not configured yet, not flushing\n");
        }
    }

    /* Update the latest window buffer for the wayland surface. Note that we
     * only care whether the buffer contains the latest window contents,
     * it's irrelevant if it was actually committed or not. */
    if (data->window_contents)
        wayland_shm_buffer_unref(data->window_contents);
    wayland_shm_buffer_ref((data->window_contents = shm_buffer));

    mapped = !mapped && get_mapped_toplevel(data);
    wayland_win_data_release(data);

    /* the toplevel got its first buffer, it can be the parent of the windows it owns now */
    if (mapped) NtUserPostMessage(hwnd, WM_WAYLAND_UPDATE_XDG_PARENTS, 0, 0);

    return committed;
}

struct wayland_shm_buffer *get_window_surface_contents(HWND hwnd)
{
    struct wayland_shm_buffer *shm_buffer;
    struct wayland_win_data *data;

    if (!(data = wayland_win_data_get(hwnd))) return NULL;
    if ((shm_buffer = data->window_contents)) wayland_shm_buffer_ref(shm_buffer);
    wayland_win_data_release(data);

    return shm_buffer;
}

void ensure_window_surface_contents(HWND hwnd)
{
    struct wayland_surface *wayland_surface;
    struct wayland_win_data *data;
    BOOL mapped;

    if (!(data = wayland_win_data_get(hwnd))) return;
    mapped = get_mapped_toplevel(data) != NULL;

    if ((wayland_surface = data->wayland_surface))
    {
        wayland_surface_ensure_contents(wayland_surface);

        /* Handle any processed configure request, to ensure the related
         * surface state is applied by the compositor. */
        if (wayland_surface->processing.serial &&
            wayland_surface->processing.processed &&
            wayland_surface_reconfigure(wayland_surface))
        {
            wl_surface_commit(wayland_surface->wl_surface);
        }
    }

    mapped = !mapped && get_mapped_toplevel(data);
    wayland_win_data_release(data);

    if (mapped) NtUserPostMessage(hwnd, WM_WAYLAND_UPDATE_XDG_PARENTS, 0, 0);
}
