/*
 * Copyright 2020 Nikolay Sivov for CodeWeavers
 * Copyright 2026 Giang Nguyen
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

#include <stdarg.h>
#include <stdio.h>

#define INITGUID
#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "objidl.h"
#include "dxgi.h"
#include "d3d11.h"
#include "d3d11_4.h"
#include "d2d1_1.h"
#include "dcomp.h"
#include "wine/debug.h"
#include "wine/winedxgi.h"
#include "wine/dcomp_layer.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

/* Private heap for dcomp — isolates surface/visual/target lifecycle
 * allocations (BeginDraw/EndDraw cycles, dirty-rect tracking, persistent
 * context wrappers) from the process heap. */
static HANDLE dcomp_heap;

static inline void *dcomp_private_alloc(size_t size)
{
    return HeapAlloc(dcomp_heap, 0, size);
}

static inline void *dcomp_private_calloc(size_t count, size_t size)
{
    if (size && count > (~(size_t)0) / size)
        return NULL;
    return HeapAlloc(dcomp_heap, HEAP_ZERO_MEMORY, count * size);
}

static inline void *dcomp_private_realloc(void *ptr, size_t size)
{
    if (!ptr) return HeapAlloc(dcomp_heap, 0, size);
    return HeapReAlloc(dcomp_heap, 0, ptr, size);
}

static inline void dcomp_private_free(void *ptr)
{
    if (ptr) HeapFree(dcomp_heap, 0, ptr);
}

#define malloc(s)       dcomp_private_alloc(s)
#define calloc(c, s)    dcomp_private_calloc(c, s)
#define realloc(p, s)   dcomp_private_realloc(p, s)
#define free(p)         dcomp_private_free(p)

#define WM_WINE_DCOMP_SET_CHILD_MODE (WM_USER + 0x101)
/* dxgi posts this to a target window after each successful HWND present so
 * the subclassed wndproc can put the delivered leaves back over the presented
 * frame without waiting for the next tree timer tick (issue 206). */
#define WM_WINE_DCOMP_PRESENT_FLUSH (WM_USER + 0x102)

/* Forward declarations for target back-pointer and list management */
struct dcomp_device;
struct dcomp_target;
static void dcomp_device_remove_target(struct dcomp_device *device, struct dcomp_target *target);

/* Send child mode message to a swapchain's comp window */
/* Periodic tree compositing for rootless trees (Chromium/WebView2, issue 88):
 * the root visual carries no content, the swapchains hang on nested leaf
 * visuals, so no root Present ever composites them. The timer drives the
 * target-side composite in dcomp_target_composite_tree().
 *
 * The timer is meant as a backstop behind the hook-driven composite below, but
 * Chromium commits the tree exactly once -- at build-up -- and never again
 * (measured: 1 commit in 265 s against 2600 composites).  The hook therefore
 * stops firing after the first frame and this timer alone paces the display,
 * which makes its period the frame rate: at 100 ms every WebView2 view ran at
 * a hard 10 Hz.  The cost of 16 ms sits in Chromium's readback, not in the
 * composite (0.0% either way), and does not scale with the number of targets:
 * +17 pp for one view (UVI Portal), +9.9 pp for two (Live 12). */
#define DCOMP_TREE_TIMER     ((UINT_PTR)0xDC0FFEE2)
#define DCOMP_TREE_TIMER_MS  16
#define DCOMP_TREE_FRAME_MS  16   /* ~60 Hz rate limit for hook-driven composites */

/* Measurement dial (issue 206): the tree timer period, adjustable without a
 * rebuild so the delivery rate can be varied against the application's present
 * rate.  WINE_DCOMP_TREE_TIMER_MS=<ms>, default 16 = unchanged behaviour. */
static UINT dcomp_tree_timer_ms(void)
{
    static int cached = -1;

    if (cached < 0)
    {
        const char *env = getenv("WINE_DCOMP_TREE_TIMER_MS");

        cached = (env && atoi(env) > 0) ? atoi(env) : DCOMP_TREE_TIMER_MS;
    }
    return cached;
}

/* Present-driven delivery (issue 206): a present to the target window
 * overwrites whatever the GDI delivery last put there, and between that
 * present and our next tree-timer blit the leaves are gone -- measured as a
 * miss rate linear in the delivery interval (see issue 206).  On by default;
 * WINE_DCOMP_PRESENT_DRIVEN=0 restores pure timer behaviour. */
static BOOL dcomp_present_driven(void)
{
    static int cached = -1;

    if (cached < 0)
    {
        const char *env = getenv("WINE_DCOMP_PRESENT_DRIVEN");

        cached = (env && !atoi(env)) ? 0 : 1;
    }
    return cached;
}
/* How long the target window may show something other than what we delivered
 * before we re-deliver it.  A tab switch legitimately repaints the panel while
 * Chromium's hide is still in flight (~100-500 ms), and re-blitting into that
 * race is exactly the tab bleed the unchanged-content gate prevents.  A window
 * that lost our pixels to an unrelated sibling stays wrong indefinitely
 * (measured: phases of 8-19 s).  Both look identical pixel-wise, so the
 * duration is what separates them; measured phase lengths cluster below 620 ms
 * and above 790 ms, and this sits in that gap. */
#define DCOMP_TARGET_REPAIR_MS 700

static void dcomp_send_child_mode(IUnknown *content)
{
    WCHAR prop_name[64];
    HWND comp_wnd;

    if (!content)
        return;

    swprintf(prop_name, ARRAY_SIZE(prop_name),
            WINE_DCOMP_WND_PROP_FMT, GetCurrentProcessId(), (UINT_PTR)content);
    comp_wnd = (HWND)GetPropW(GetDesktopWindow(), prop_name);
    if (comp_wnd)
    {
        FIXME("Sending child mode to comp_wnd %p for content %p.\n", comp_wnd, content);
        SendMessageW(comp_wnd, WM_WINE_DCOMP_SET_CHILD_MODE, 0, 0);
    }
}

static HRESULT STDMETHODCALLTYPE dcomp_device_Commit(IDCompositionDevice *iface);
static LONGLONG dcomp_qpc_now(void);
static LONGLONG dcomp_qpc_freq(void);
static void dcomp_device_auto_commit(IDCompositionDevice *iface);

/* =====================================================================
 * IDCompositionSurface
 * ===================================================================== */

struct dcomp_surface
{
    IDCompositionSurface IDCompositionSurface_iface;
    LONG refcount;
    UINT width, height;
    DXGI_FORMAT format;
    DXGI_ALPHA_MODE alpha_mode;
    BOOL is_virtual;          /* TRUE for IDCompositionVirtualSurface */
    /* D3D11 path (when rendering_device is IDXGIDevice/ID3D11Device) */
    ID3D11Device *d3d11_device;
    ID3D11Texture2D *texture;         /* GPU render target */
    ID3D11Texture2D *staging;         /* CPU-readable staging copy */
    IDXGISurface *dxgi_surface;       /* QI from texture */
    /* D2D1 bitmap path (when rendering_device is ID2D1Device) */
    ID2D1Device *d2d1_device;                /* VSTGUI's D2D1 device — resource-compatible */
    ID2D1DeviceContext *persistent_context;   /* reusable DC for BeginDraw/EndDraw */
    ID2D1Bitmap1 *target_bitmap;             /* GPU render target (D2D1_BITMAP_OPTIONS_TARGET) */
    ID2D1Bitmap1 *readback_bitmap;           /* CPU-readable (CPU_READ | CANNOT_DRAW) */
    /* Shared state */
    DWORD *bits;                      /* System memory for composition */
    BOOL drawing;
    ID2D1DeviceContext *active_context; /* D2D1 context from BeginDraw (for EndDraw clip pop) */
    BOOL has_clip;                     /* TRUE if PushAxisAlignedClip active */
    IDCompositionDevice *device_iface; /* back-pointer for auto-commit in EndDraw */
    /* Dirty-rect tracking — avoids full-surface GPU readback per frame.
     * Without this, EndDraw copies width*height pixels GPU→CPU even when
     * the app only redrew a small area (e.g. one knob). */
    BOOL has_dirty_rect;               /* TRUE if BeginDraw got a valid sub-rect */
    RECT dirty_rect;                   /* clamped to surface bounds */
    BOOL needs_full_init_copy;         /* TRUE before first readback — must copy full surface once */
    /* Readback coalescing (issue 56): the GPU→CPU Map in a readback forces a ~1.3ms GPU
     * sync stall.  A fast drag does ~10 EndDraws/frame (~500/s) → the render thread would
     * stall ~650ms/s.  So EndDraw only ACCUMULATES the dirty region here and defers the
     * readback to the throttled present (~60/s). */
    RECT pending_dirty;                /* union of dirty rects awaiting readback+present */
    BOOL has_pending;                  /* GPU has content not yet read back to surface->bits */
};

static inline struct dcomp_surface *impl_from_IDCompositionSurface(IDCompositionSurface *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_surface, IDCompositionSurface_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_QueryInterface(IDCompositionSurface *iface,
        REFIID iid, void **out)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurface(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionSurface))
    {
        *out = &surface->IDCompositionSurface_iface;
        IDCompositionSurface_AddRef(*out);
        return S_OK;
    }

    if (surface->is_virtual && IsEqualGUID(iid, &IID_IDCompositionVirtualSurface))
    {
        *out = &surface->IDCompositionSurface_iface;
        IDCompositionSurface_AddRef(*out);
        return S_OK;
    }

    FIXME("unsupported iid %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dcomp_surface_AddRef(IDCompositionSurface *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurface(iface);
    ULONG refcount = InterlockedIncrement(&surface->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);
    return refcount;
}

static ULONG STDMETHODCALLTYPE dcomp_surface_Release(IDCompositionSurface *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurface(iface);
    ULONG refcount = InterlockedDecrement(&surface->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        /* D2D1 bitmap path cleanup */
        if (surface->readback_bitmap)
            ID2D1Bitmap1_Release(surface->readback_bitmap);
        if (surface->target_bitmap)
            ID2D1Bitmap1_Release(surface->target_bitmap);
        if (surface->persistent_context)
            ID2D1DeviceContext_Release(surface->persistent_context);
        if (surface->d2d1_device)
            ID2D1Device_Release(surface->d2d1_device);
        /* D3D11 path cleanup */
        if (surface->dxgi_surface)
            IDXGISurface_Release(surface->dxgi_surface);
        if (surface->staging)
            ID3D11Texture2D_Release(surface->staging);
        if (surface->texture)
            ID3D11Texture2D_Release(surface->texture);
        if (surface->d3d11_device)
            ID3D11Device_Release(surface->d3d11_device);
        /* Drop the back-reference taken in dcomp_device_Create[Virtual]Surface. */
        if (surface->device_iface)
            IDCompositionDevice_Release(surface->device_iface);
        free(surface->bits);
        free(surface);
    }
    return refcount;
}

/* An application that draws through IDXGISurface1::GetDC needs the texture
 * behind the surface created for it, and D3D11 only accepts the flag on a
 * B8G8R8A8 texture of default usage.  Ask for it exactly where it is allowed:
 * requesting it elsewhere fails CreateTexture2D outright and would cost the
 * surface altogether.  Where it is allowed it is free until someone calls
 * GetDC -- wined3d only records the capability and builds the DIB on demand. */
static UINT dcomp_surface_gdi_flag(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
        default:
            return 0;
    }
}

/* The render-target and CPU-readable bitmaps of a D2D1-backed surface at the
 * given size.  Shared by the lazy first-use path below and by Resize, which
 * needs the new pair while the old one still exists so that the contents can
 * be carried across. */
static HRESULT dcomp_surface_create_d2d1_bitmaps(ID2D1DeviceContext *context, UINT width, UINT height,
        ID2D1Bitmap1 **target_bitmap, ID2D1Bitmap1 **readback_bitmap)
{
    D2D1_BITMAP_PROPERTIES1 bmp_props;
    D2D1_SIZE_U size;
    HRESULT hr;

    *target_bitmap = NULL;
    *readback_bitmap = NULL;

    size.width = width;
    size.height = height;

    /* Render-target bitmap (GPU-backed, TARGET flag) */
    memset(&bmp_props, 0, sizeof(bmp_props));
    bmp_props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bmp_props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bmp_props.dpiX = 96.0f;
    bmp_props.dpiY = 96.0f;
    bmp_props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;

    hr = ID2D1DeviceContext_CreateBitmap(context, size, NULL, 0, &bmp_props, target_bitmap);
    if (FAILED(hr))
    {
        FIXME("CreateBitmap(TARGET) failed: %#lx.\n", hr);
        *target_bitmap = NULL;
        return hr;
    }

    /* CPU-readable bitmap for readback (CANNOT_DRAW | CPU_READ) */
    bmp_props.bitmapOptions = D2D1_BITMAP_OPTIONS_CANNOT_DRAW | D2D1_BITMAP_OPTIONS_CPU_READ;

    hr = ID2D1DeviceContext_CreateBitmap(context, size, NULL, 0, &bmp_props, readback_bitmap);
    if (FAILED(hr))
    {
        FIXME("CreateBitmap(CPU_READ) failed: %#lx.\n", hr);
        ID2D1Bitmap1_Release(*target_bitmap);
        *target_bitmap = NULL;
        *readback_bitmap = NULL;
        return hr;
    }

    return S_OK;
}

/* Lazy-init the persistent D2D1 context and bitmaps for the D2D1Device path. */
static HRESULT dcomp_surface_ensure_d2d1_resources(struct dcomp_surface *surface)
{
    HRESULT hr;

    if (surface->persistent_context)
        return S_OK;   /* already initialised */

    hr = ID2D1Device_CreateDeviceContext(surface->d2d1_device,
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &surface->persistent_context);
    if (FAILED(hr))
    {
        FIXME("ID2D1Device_CreateDeviceContext failed: %#lx.\n", hr);
        return hr;
    }

    hr = dcomp_surface_create_d2d1_bitmaps(surface->persistent_context, surface->width, surface->height,
            &surface->target_bitmap, &surface->readback_bitmap);
    if (FAILED(hr))
    {
        ID2D1DeviceContext_Release(surface->persistent_context);
        surface->persistent_context = NULL;
        return hr;
    }

    FIXME("Created D2D1 resources: context %p, target_bitmap %p, readback_bitmap %p (%ux%u).\n",
            surface->persistent_context, surface->target_bitmap,
            surface->readback_bitmap, surface->width, surface->height);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_BeginDraw(IDCompositionSurface *iface,
        const RECT *rect, REFIID iid, void **object, POINT *offset)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurface(iface);
    ID2D1DeviceContext *context;
    HRESULT hr;

    TRACE("iface %p, rect %s, iid %s, object %p, offset %p.\n",
            iface, wine_dbgstr_rect(rect), debugstr_guid(iid), object, offset);

    if (!object || !offset)
        return E_INVALIDARG;

    *object = NULL;

    if (surface->drawing)
    {
        WARN("Already in a draw session.\n");
        return E_FAIL;
    }

    /* Remember the dirty rect (clamped) so EndDraw can avoid a full-surface readback. */
    if (rect && (rect->right > rect->left) && (rect->bottom > rect->top))
    {
        surface->dirty_rect = *rect;
        if (surface->dirty_rect.left < 0) surface->dirty_rect.left = 0;
        if (surface->dirty_rect.top  < 0) surface->dirty_rect.top  = 0;
        if (surface->dirty_rect.right  > (LONG)surface->width)
            surface->dirty_rect.right  = (LONG)surface->width;
        if (surface->dirty_rect.bottom > (LONG)surface->height)
            surface->dirty_rect.bottom = (LONG)surface->height;
        surface->has_dirty_rect = (surface->dirty_rect.right > surface->dirty_rect.left
                                && surface->dirty_rect.bottom > surface->dirty_rect.top);
    }
    else
    {
        surface->has_dirty_rect = FALSE;
    }

    if (IsEqualGUID(iid, &IID_ID2D1DeviceContext))
    {
        /* D2D1Device path: use VSTGUI's device so resources are compatible. */
        if (surface->d2d1_device)
        {
            hr = dcomp_surface_ensure_d2d1_resources(surface);
            if (FAILED(hr))
                return hr;

            context = surface->persistent_context;
            ID2D1DeviceContext_SetTarget(context, (ID2D1Image *)surface->target_bitmap);
            ID2D1DeviceContext_BeginDraw(context);
        }
        else if (surface->dxgi_surface)
        {
            /* D3D11 path: create a standalone D2D1 context from the DXGI surface. */
            D2D1_CREATION_PROPERTIES props;
            memset(&props, 0, sizeof(props));
            props.threadingMode = D2D1_THREADING_MODE_SINGLE_THREADED;
            props.options = D2D1_DEVICE_CONTEXT_OPTIONS_NONE;

            hr = D2D1CreateDeviceContext(surface->dxgi_surface, &props, &context);
            if (FAILED(hr))
            {
                FIXME("D2D1CreateDeviceContext failed: %#lx.\n", hr);
                return hr;
            }
        }
        else
        {
            WARN("Surface has neither D2D1 device nor DXGI surface.\n");
            return E_FAIL;
        }

        /* Push a clip rect so that Clear() only affects the dirty area,
         * preserving the rest of the surface (VSTGUI partial redraw pattern). */
        if (surface->has_dirty_rect)
        {
            D2D1_RECT_F clip_rect;
            clip_rect.left = (float)surface->dirty_rect.left;
            clip_rect.top = (float)surface->dirty_rect.top;
            clip_rect.right = (float)surface->dirty_rect.right;
            clip_rect.bottom = (float)surface->dirty_rect.bottom;
            ID2D1DeviceContext_PushAxisAlignedClip(context, &clip_rect,
                    D2D1_ANTIALIAS_MODE_ALIASED);
            surface->has_clip = TRUE;
            TRACE("Pushed dirty-rect clip (%ld,%ld)-(%ld,%ld) on context %p.\n",
                    surface->dirty_rect.left, surface->dirty_rect.top,
                    surface->dirty_rect.right, surface->dirty_rect.bottom, context);
        }
        else
        {
            surface->has_clip = FALSE;
        }

        /* Keep a reference for EndDraw to pop the clip */
        surface->active_context = context;
        ID2D1DeviceContext_AddRef(context);

        ID2D1DeviceContext_AddRef(context);  /* AddRef for caller (COM out-parameter) */
        *object = context;
        surface->drawing = TRUE;
        offset->x = rect ? rect->left : 0;
        offset->y = rect ? rect->top : 0;
        return S_OK;
    }

    /* IDXGISurface1 adds GetDC()/ReleaseDC() and is how an application draws
     * into a composition surface with GDI.  Fender Studio Pro 8 asks for
     * nothing else: its entire visual tree is the transport playhead, three
     * one-pixel leaves, and it opens every one of them this way.  Refusing the
     * interface left those leaves empty for good -- measured over three
     * sessions, 16688 to 41202 calls, every one of them turned away and not a
     * single EndDraw in return, which is why the playhead was never drawn. */
    if (IsEqualGUID(iid, &IID_IDXGISurface)
            || IsEqualGUID(iid, &IID_IDXGISurface1)
            || IsEqualGUID(iid, &IID_IDXGISurface2))
    {
        if (!surface->dxgi_surface)
        {
            WARN("Surface has no DXGI surface.\n");
            return E_FAIL;
        }
        if (FAILED(hr = IDXGISurface_QueryInterface(surface->dxgi_surface, iid, object)))
        {
            WARN("Surface does not support %s: %#lx.\n", debugstr_guid(iid), hr);
            return hr;
        }
        surface->drawing = TRUE;
        offset->x = rect ? rect->left : 0;
        offset->y = rect ? rect->top : 0;
        return S_OK;
    }

    if (IsEqualGUID(iid, &IID_ID3D11Texture2D))
    {
        /* Chromium's DCompSurfaceImageBacking draws through BeginDraw with
         * IID_ID3D11Texture2D (issue 95).  Hand out the persistent D3D11
         * render target; EndDraw accumulates the dirty region and the
         * throttled present reads it back like the DXGI-surface path. */
        if (!surface->texture)
        {
            WARN("Surface has no D3D11 texture.\n");
            return E_FAIL;
        }
        ID3D11Texture2D_AddRef(surface->texture);
        *object = surface->texture;
        surface->drawing = TRUE;
        offset->x = rect ? rect->left : 0;
        offset->y = rect ? rect->top : 0;
        return S_OK;
    }

    FIXME("Unsupported IID %s for BeginDraw.\n", debugstr_guid(iid));
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_EndDraw(IDCompositionSurface *iface)
{
    struct dcomp_surface *surface = impl_from_IDCompositionSurface(iface);
    HRESULT hr;

    TRACE("iface %p.\n", iface);

    if (!surface->drawing)
    {
        WARN("Not in a draw session.\n");
        return E_FAIL;
    }

    /* Pop the dirty-rect clip before readback so the
     * D2D1 command list is finalized correctly. */
    if (surface->active_context)
    {
        if (surface->has_clip)
        {
            ID2D1DeviceContext_PopAxisAlignedClip(surface->active_context);
            surface->has_clip = FALSE;
            TRACE("Popped dirty-rect clip on context %p.\n", surface->active_context);
        }
        ID2D1DeviceContext_Release(surface->active_context);
        surface->active_context = NULL;
    }

    surface->drawing = FALSE;

    /* D2D command finalization must run per draw cycle (cheap, ~0us).  The expensive
     * GPU→CPU readback (~1.3ms GPU sync stall) is deferred to the throttled present
     * (issue 56) — here we only accumulate the dirty region into surface->pending_dirty. */
    if (surface->d2d1_device && surface->persistent_context)
    {
        hr = ID2D1DeviceContext_EndDraw(surface->persistent_context, NULL, NULL);
        if (FAILED(hr))
            FIXME("ID2D1DeviceContext_EndDraw failed: %#lx.\n", hr);
    }

    {
        RECT d;
        if (surface->has_dirty_rect && !surface->needs_full_init_copy)
            d = surface->dirty_rect;
        else
            SetRect(&d, 0, 0, (LONG)surface->width, (LONG)surface->height);
        if (surface->has_pending)
            UnionRect(&surface->pending_dirty, &surface->pending_dirty, &d);
        else
            surface->pending_dirty = d;
        surface->has_pending = TRUE;
    }

    /* Auto-commit: present the updated surface immediately so that apps
     * that don't call Commit() after every EndDraw (or call it too late)
     * still get timely screen updates.  This mirrors Windows behavior where
     * DWM presents composition changes at the next vsync boundary.
     * Reentrancy/per-device guard handled in dcomp_device_auto_commit. */
    if (surface->device_iface)
        dcomp_device_auto_commit(surface->device_iface);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_SuspendDraw(IDCompositionSurface *iface)
{
    FIXME("iface %p stub!\n", iface);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_ResumeDraw(IDCompositionSurface *iface)
{
    FIXME("iface %p stub!\n", iface);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_Scroll(IDCompositionSurface *iface,
        const RECT *scroll, const RECT *clip, int offset_x, int offset_y)
{
    FIXME("iface %p, scroll %s, clip %s, offset %d,%d stub!\n",
            iface, wine_dbgstr_rect(scroll), wine_dbgstr_rect(clip),
            offset_x, offset_y);
    return S_OK;
}

static const IDCompositionSurfaceVtbl dcomp_surface_vtbl =
{
    dcomp_surface_QueryInterface,
    dcomp_surface_AddRef,
    dcomp_surface_Release,
    dcomp_surface_BeginDraw,
    dcomp_surface_EndDraw,
    dcomp_surface_SuspendDraw,
    dcomp_surface_ResumeDraw,
    dcomp_surface_Scroll,
};

/* IDCompositionVirtualSurface extends IDCompositionSurface */
static HRESULT STDMETHODCALLTYPE dcomp_virtual_surface_Resize(IDCompositionVirtualSurface *iface,
        UINT width, UINT height)
{
    struct dcomp_surface *surface = CONTAINING_RECORD(iface, struct dcomp_surface, IDCompositionSurface_iface);
    ID3D11Texture2D *new_texture = NULL, *new_staging = NULL;
    ID2D1Bitmap1 *new_target_bitmap = NULL, *new_readback_bitmap = NULL;
    IDXGISurface *new_dxgi_surface = NULL;
    UINT copy_w, copy_h, y;
    DWORD *new_bits;

    TRACE("iface %p, %ux%u (old %ux%u).\n", iface, width, height, surface->width, surface->height);

    if (width == surface->width && height == surface->height)
        return S_OK;

    if (surface->drawing)
    {
        WARN("Resize called during active BeginDraw — ignoring.\n");
        return DXGI_ERROR_INVALID_CALL;
    }

    /* Resize in phases: create ALL new resources first, then carry the old
     * contents across, and only when everything succeeded release the old
     * resources and swap in the new size.  On any failure the surface is left
     * fully intact (old bits/textures/dimensions), so a failed Resize cannot
     * leave new-size metadata with missing GPU objects (which would make later
     * draws fail unpredictably). */

    /* Phase 1 — allocate/create new resources without touching the surface. */
    new_bits = calloc(width * height, sizeof(DWORD));
    if (!new_bits)
        return E_OUTOFMEMORY;

    if (surface->d3d11_device)
    {
        D3D11_TEXTURE2D_DESC tex_desc;
        HRESULT hr;

        memset(&tex_desc, 0, sizeof(tex_desc));
        tex_desc.Width = width;
        tex_desc.Height = height;
        tex_desc.MipLevels = 1;
        tex_desc.ArraySize = 1;
        tex_desc.Format = surface->format;
        tex_desc.SampleDesc.Count = 1;
        tex_desc.Usage = D3D11_USAGE_DEFAULT;
        tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        tex_desc.MiscFlags = dcomp_surface_gdi_flag(surface->format);

        hr = ID3D11Device_CreateTexture2D(surface->d3d11_device, &tex_desc, NULL, &new_texture);
        if (FAILED(hr))
        {
            WARN("Failed to create resized render target texture: %#lx.\n", hr);
            free(new_bits);
            return hr;
        }

        tex_desc.Usage = D3D11_USAGE_STAGING;
        tex_desc.BindFlags = 0;
        tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        /* GDI compatibility is a property of the drawable copy; a staging
         * texture carrying the flag fails validation for lack of default usage. */
        tex_desc.MiscFlags = 0;

        hr = ID3D11Device_CreateTexture2D(surface->d3d11_device, &tex_desc, NULL, &new_staging);
        if (FAILED(hr))
        {
            WARN("Failed to create resized staging texture: %#lx.\n", hr);
            ID3D11Texture2D_Release(new_texture);
            free(new_bits);
            return hr;
        }

        hr = ID3D11Texture2D_QueryInterface(new_texture,
                &IID_IDXGISurface, (void **)&new_dxgi_surface);
        if (FAILED(hr))
        {
            WARN("Failed to get IDXGISurface from resized texture: %#lx.\n", hr);
            ID3D11Texture2D_Release(new_staging);
            ID3D11Texture2D_Release(new_texture);
            free(new_bits);
            return hr;
        }
    }

    /* On the D2D1 path the drawn pixels live in target_bitmap alone -- there is
     * no D3D11 texture behind it.  Create the new pair here instead of leaving
     * it to the next BeginDraw, so the old contents can be carried across
     * below.  Should that fail, the lazy path still applies: the bitmaps go and
     * BeginDraw re-creates them empty, as before. */
    if (surface->d2d1_device && surface->persistent_context && surface->target_bitmap
            && FAILED(dcomp_surface_create_d2d1_bitmaps(surface->persistent_context, width, height,
                    &new_target_bitmap, &new_readback_bitmap)))
        WARN("Could not create the resized D2D1 bitmaps; the contents will not be preserved.\n");

    /* Phase 2 -- "When a virtual surface is resized, its contents are preserved
     * up to the new boundaries of the surface" (IDCompositionVirtualSurface::Resize).
     * An application may therefore repaint only what the new layout moved.
     * Cubase 15 does exactly that on the project window's root surface: after
     * a resize it redraws the panes below the toolbar and the transport bar,
     * and the toolbar strip, never drawn again, stayed black here because the
     * new texture and bits started out empty. */
    copy_w = min(width, surface->width);
    copy_h = min(height, surface->height);
    if (copy_w && copy_h && surface->bits)
    {
        for (y = 0; y < copy_h; ++y)
            memcpy(new_bits + y * width, surface->bits + y * surface->width, copy_w * sizeof(DWORD));

        if (new_texture && surface->texture)
        {
            ID3D11DeviceContext *d3d_context;
            D3D11_BOX box;

            box.left = 0; box.top = 0; box.front = 0;
            box.right = copy_w; box.bottom = copy_h; box.back = 1;
            ID3D11Device_GetImmediateContext(surface->d3d11_device, &d3d_context);
            ID3D11DeviceContext_CopySubresourceRegion(d3d_context, (ID3D11Resource *)new_texture, 0,
                    0, 0, 0, (ID3D11Resource *)surface->texture, 0, &box);
            ID3D11DeviceContext_Release(d3d_context);
        }

        if (new_target_bitmap)
        {
            D2D1_POINT_2U dst_point = {0, 0};
            D2D1_RECT_U src_rect;
            HRESULT hr;

            src_rect.left = 0; src_rect.top = 0;
            src_rect.right = copy_w; src_rect.bottom = copy_h;
            hr = ID2D1Bitmap1_CopyFromBitmap(new_target_bitmap, &dst_point,
                    (ID2D1Bitmap *)surface->target_bitmap, &src_rect);
            if (FAILED(hr))
                WARN("Could not copy the old contents into the resized bitmap: %#lx.\n", hr);
        }
    }

    /* Phase 3 -- everything created and copied: release the old resources and
     * swap in the new. */
    free(surface->bits);
    surface->bits = new_bits;

    if (surface->readback_bitmap)
    {
        ID2D1Bitmap1_Release(surface->readback_bitmap);
        surface->readback_bitmap = NULL;
    }
    if (surface->target_bitmap)
    {
        ID2D1Bitmap1_Release(surface->target_bitmap);
        surface->target_bitmap = NULL;
    }
    if (new_target_bitmap)
    {
        /* The device context is not bound to a size; it keeps serving the new
         * pair, and BeginDraw sets the target as it always did. */
        surface->target_bitmap = new_target_bitmap;
        surface->readback_bitmap = new_readback_bitmap;
    }
    else if (surface->persistent_context)
    {
        /* Re-created lazily in BeginDraw via dcomp_surface_ensure_d2d1_resources(). */
        ID2D1DeviceContext_Release(surface->persistent_context);
        surface->persistent_context = NULL;
    }

    if (surface->d3d11_device)
    {
        if (surface->dxgi_surface)
            IDXGISurface_Release(surface->dxgi_surface);
        if (surface->staging)
            ID3D11Texture2D_Release(surface->staging);
        if (surface->texture)
            ID3D11Texture2D_Release(surface->texture);
        surface->texture = new_texture;
        surface->staging = new_staging;
        surface->dxgi_surface = new_dxgi_surface;
    }

    surface->width = width;
    surface->height = height;
    surface->needs_full_init_copy = TRUE;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_virtual_surface_Trim(IDCompositionVirtualSurface *iface,
        const RECT *rectangles, UINT count)
{
    FIXME("iface %p, rectangles %p, count %u stub!\n", iface, rectangles, count);
    return S_OK;
}

static const IDCompositionVirtualSurfaceVtbl dcomp_virtual_surface_vtbl =
{
    /* IDCompositionSurface methods */
    (void *)dcomp_surface_QueryInterface,
    (void *)dcomp_surface_AddRef,
    (void *)dcomp_surface_Release,
    (void *)dcomp_surface_BeginDraw,
    (void *)dcomp_surface_EndDraw,
    (void *)dcomp_surface_SuspendDraw,
    (void *)dcomp_surface_ResumeDraw,
    (void *)dcomp_surface_Scroll,
    /* IDCompositionVirtualSurface methods */
    dcomp_virtual_surface_Resize,
    dcomp_virtual_surface_Trim,
};

static HRESULT dcomp_surface_create(ID3D11Device *d3d11_device, ID2D1Device *d2d1_device,
        UINT width, UINT height, DXGI_FORMAT pixel_format,
        DXGI_ALPHA_MODE alpha_mode, BOOL is_virtual,
        struct dcomp_surface **out)
{
    struct dcomp_surface *surface;
    HRESULT hr;

    if (!(surface = calloc(1, sizeof(*surface))))
        return E_OUTOFMEMORY;

    surface->IDCompositionSurface_iface.lpVtbl = is_virtual
            ? (const IDCompositionSurfaceVtbl *)&dcomp_virtual_surface_vtbl
            : &dcomp_surface_vtbl;
    surface->refcount = 1;
    surface->width = width;
    surface->height = height;
    surface->format = pixel_format;
    surface->alpha_mode = alpha_mode;
    surface->is_virtual = is_virtual;
    surface->needs_full_init_copy = TRUE;

    if (d2d1_device)
    {
        /* D2D1 bitmap path: store device reference, bitmaps created lazily in BeginDraw.
         * This path is used when the rendering_device is an ID2D1Device (VSTGUI pattern).
         * We create the DeviceContext and bitmaps from this device so that D2D1 resources
         * (brushes, bitmaps, effects) created by VSTGUI are compatible with our context. */
        surface->d2d1_device = d2d1_device;
        ID2D1Device_AddRef(d2d1_device);
    }
    else if (d3d11_device)
    {
        /* D3D11 path: create GPU textures for rendering + staging for readback. */
        D3D11_TEXTURE2D_DESC tex_desc;

        surface->d3d11_device = d3d11_device;
        ID3D11Device_AddRef(d3d11_device);

        memset(&tex_desc, 0, sizeof(tex_desc));
        tex_desc.Width = width;
        tex_desc.Height = height;
        tex_desc.MipLevels = 1;
        tex_desc.ArraySize = 1;
        tex_desc.Format = pixel_format;
        tex_desc.SampleDesc.Count = 1;
        tex_desc.Usage = D3D11_USAGE_DEFAULT;
        tex_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        tex_desc.MiscFlags = dcomp_surface_gdi_flag(pixel_format);

        hr = ID3D11Device_CreateTexture2D(d3d11_device, &tex_desc, NULL, &surface->texture);
        if (FAILED(hr))
        {
            FIXME("Failed to create render target texture: %#lx.\n", hr);
            goto fail;
        }

        tex_desc.Usage = D3D11_USAGE_STAGING;
        tex_desc.BindFlags = 0;
        tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        /* GDI compatibility is a property of the drawable copy; a staging
         * texture carrying the flag fails validation for lack of default usage. */
        tex_desc.MiscFlags = 0;

        hr = ID3D11Device_CreateTexture2D(d3d11_device, &tex_desc, NULL, &surface->staging);
        if (FAILED(hr))
        {
            FIXME("Failed to create staging texture: %#lx.\n", hr);
            goto fail;
        }

        hr = ID3D11Texture2D_QueryInterface(surface->texture,
                &IID_IDXGISurface, (void **)&surface->dxgi_surface);
        if (FAILED(hr))
        {
            FIXME("Failed to get IDXGISurface from texture: %#lx.\n", hr);
            goto fail;
        }
    }
    else
    {
        FIXME("No rendering device provided.\n");
        free(surface);
        return E_INVALIDARG;
    }

    /* Allocate system memory for composition (transparent = 0x00000000) */
    surface->bits = calloc(width * height, sizeof(DWORD));
    if (!surface->bits)
    {
        hr = E_OUTOFMEMORY;
        goto fail;
    }

    FIXME("Created DComp surface %p (%ux%u, fmt=%#x, alpha=%#x, virtual=%d, d2d1=%d).\n",
            surface, width, height, pixel_format, alpha_mode, is_virtual, !!d2d1_device);

    *out = surface;
    return S_OK;

fail:
    IDCompositionSurface_Release(&surface->IDCompositionSurface_iface);
    return hr;
}

/* =====================================================================
 * IDCompositionSurfaceFactory
 * ===================================================================== */

struct dcomp_surface_factory
{
    IDCompositionSurfaceFactory IDCompositionSurfaceFactory_iface;
    LONG refcount;
    IDCompositionDevice *device_iface;  /* back-pointer via COM interface */
};

static inline struct dcomp_surface_factory *impl_from_IDCompositionSurfaceFactory(
        IDCompositionSurfaceFactory *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_surface_factory, IDCompositionSurfaceFactory_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_factory_QueryInterface(
        IDCompositionSurfaceFactory *iface, REFIID iid, void **out)
{
    struct dcomp_surface_factory *factory = impl_from_IDCompositionSurfaceFactory(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionSurfaceFactory))
    {
        *out = &factory->IDCompositionSurfaceFactory_iface;
        IDCompositionSurfaceFactory_AddRef(*out);
        return S_OK;
    }

    FIXME("unsupported iid %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dcomp_surface_factory_AddRef(IDCompositionSurfaceFactory *iface)
{
    struct dcomp_surface_factory *factory = impl_from_IDCompositionSurfaceFactory(iface);
    ULONG refcount = InterlockedIncrement(&factory->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);
    return refcount;
}

static ULONG STDMETHODCALLTYPE dcomp_surface_factory_Release(IDCompositionSurfaceFactory *iface)
{
    struct dcomp_surface_factory *factory = impl_from_IDCompositionSurfaceFactory(iface);
    ULONG refcount = InterlockedDecrement(&factory->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        if (factory->device_iface)
            IDCompositionDevice_Release(factory->device_iface);
        free(factory);
    }
    return refcount;
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_factory_CreateSurface(
        IDCompositionSurfaceFactory *iface, UINT width, UINT height,
        DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    struct dcomp_surface_factory *factory = impl_from_IDCompositionSurfaceFactory(iface);

    FIXME("iface %p, %ux%u, format %#x, alpha %#x, surface %p.\n",
            iface, width, height, pixel_format, alpha_mode, surface);

    if (!factory->device_iface)
        return E_FAIL;

    return IDCompositionDevice_CreateSurface(factory->device_iface,
            width, height, pixel_format, alpha_mode, surface);
}

static HRESULT STDMETHODCALLTYPE dcomp_surface_factory_CreateVirtualSurface(
        IDCompositionSurfaceFactory *iface, UINT width, UINT height,
        DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    struct dcomp_surface_factory *factory = impl_from_IDCompositionSurfaceFactory(iface);

    FIXME("iface %p, %ux%u, format %#x, alpha %#x, surface %p.\n",
            iface, width, height, pixel_format, alpha_mode, surface);

    if (!factory->device_iface)
        return E_FAIL;

    return IDCompositionDevice_CreateVirtualSurface(factory->device_iface,
            width, height, pixel_format, alpha_mode, surface);
}

static const IDCompositionSurfaceFactoryVtbl dcomp_surface_factory_vtbl =
{
    dcomp_surface_factory_QueryInterface,
    dcomp_surface_factory_AddRef,
    dcomp_surface_factory_Release,
    dcomp_surface_factory_CreateSurface,
    dcomp_surface_factory_CreateVirtualSurface,
};

/* =====================================================================
 * IDCompositionVisual
 * ===================================================================== */

/* =====================================================================
 * IDCompositionTexture (issue 90) — raw D3D11 textures as visual content.
 *
 * Chromium's viz compositor promotes page content into a delegated-
 * compositing layer after a few frames and feeds it through composition
 * textures instead of the root swapchain. The texture object wraps the
 * app's ID3D11Texture2D and keeps a throttled CPU readback (BGRA,
 * premultiplied) that the tree-composite and serializer paths consume
 * exactly like DComp surface bits.
 * ===================================================================== */

/* Always-signaled ID3D11Fence for IDCompositionTexture::GetAvailableFence.
 * Wine's d3d11 cannot create real fences (CreateFence is a stub), but
 * Chromium CHECK-fails on any GetAvailableFence result other than
 * S_OK + non-NULL fence. Our composition readback is CPU-synchronous,
 * so the texture is always immediately available: GetCompletedValue()
 * returns the fence value handed out by GetAvailableFence (0), which
 * callers interpret as "nothing to wait for". */
struct dcomp_fence
{
    ID3D11Fence ID3D11Fence_iface;
    LONG refcount;
    ID3D11Device *device;   /* device of the wrapped texture, for GetDevice */
};

static inline struct dcomp_fence *impl_from_ID3D11Fence(ID3D11Fence *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_fence, ID3D11Fence_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_fence_QueryInterface(ID3D11Fence *iface,
        REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_ID3D11DeviceChild)
            || IsEqualGUID(iid, &IID_ID3D11Fence))
    {
        *out = iface;
        ID3D11Fence_AddRef(iface);
        return S_OK;
    }

    FIXME("unsupported iid %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dcomp_fence_AddRef(ID3D11Fence *iface)
{
    struct dcomp_fence *fence = impl_from_ID3D11Fence(iface);
    ULONG refcount = InterlockedIncrement(&fence->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);
    return refcount;
}

static ULONG STDMETHODCALLTYPE dcomp_fence_Release(ID3D11Fence *iface)
{
    struct dcomp_fence *fence = impl_from_ID3D11Fence(iface);
    ULONG refcount = InterlockedDecrement(&fence->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        if (fence->device)
            ID3D11Device_Release(fence->device);
        free(fence);
    }
    return refcount;
}

static void STDMETHODCALLTYPE dcomp_fence_GetDevice(ID3D11Fence *iface, ID3D11Device **device)
{
    struct dcomp_fence *fence = impl_from_ID3D11Fence(iface);

    TRACE("iface %p, device %p.\n", iface, device);

    *device = fence->device;
    if (*device)
        ID3D11Device_AddRef(*device);
}

static HRESULT STDMETHODCALLTYPE dcomp_fence_GetPrivateData(ID3D11Fence *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    FIXME("iface %p, guid %s, data_size %p, data %p stub!\n", iface, debugstr_guid(guid),
            data_size, data);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_fence_SetPrivateData(ID3D11Fence *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    FIXME("iface %p, guid %s, data_size %u, data %p stub!\n", iface, debugstr_guid(guid),
            data_size, data);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_fence_SetPrivateDataInterface(ID3D11Fence *iface,
        REFGUID guid, const IUnknown *data)
{
    FIXME("iface %p, guid %s, data %p stub!\n", iface, debugstr_guid(guid), data);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_fence_CreateSharedHandle(ID3D11Fence *iface,
        const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name, HANDLE *handle)
{
    FIXME("iface %p, attributes %p, access %#lx, name %s, handle %p stub!\n",
            iface, attributes, access, debugstr_w(name), handle);
    return E_NOTIMPL;
}

static UINT64 STDMETHODCALLTYPE dcomp_fence_GetCompletedValue(ID3D11Fence *iface)
{
    TRACE("iface %p.\n", iface);

    /* Matches the value GetAvailableFence reports: the texture is always
     * immediately available (CPU-synchronous composite). */
    return 0;
}

static HRESULT STDMETHODCALLTYPE dcomp_fence_SetEventOnCompletion(ID3D11Fence *iface,
        UINT64 value, HANDLE event)
{
    TRACE("iface %p, value %s, event %p.\n", iface, wine_dbgstr_longlong(value), event);

    if (event)
        SetEvent(event);
    return S_OK;
}

static const ID3D11FenceVtbl dcomp_fence_vtbl =
{
    dcomp_fence_QueryInterface,
    dcomp_fence_AddRef,
    dcomp_fence_Release,
    dcomp_fence_GetDevice,
    dcomp_fence_GetPrivateData,
    dcomp_fence_SetPrivateData,
    dcomp_fence_SetPrivateDataInterface,
    dcomp_fence_CreateSharedHandle,
    dcomp_fence_GetCompletedValue,
    dcomp_fence_SetEventOnCompletion,
};

struct dcomp_texture
{
    IDCompositionTexture IDCompositionTexture_iface;
    LONG refcount;
    ID3D11Texture2D *texture;         /* wrapped app texture */
    D3D11_TEXTURE2D_DESC desc;        /* cached at creation */
    D2D_RECT_U source_rect;
    BOOL has_source_rect;
    DXGI_ALPHA_MODE alpha_mode;
    DXGI_COLOR_SPACE_TYPE color_space;
    ID3D11Texture2D *staging;         /* cached CPU-readable copy */
    DWORD *bits;                      /* BGRA premultiplied readback (source_rect window) */
    UINT bits_width, bits_height;
    DWORD last_readback_tick;         /* ~60 Hz readback throttle */
    struct dcomp_fence *fence;        /* lazy, always-signaled */
    BOOL multithread_set;             /* ID3D11Multithread protection enabled once */
    BOOL format_warned;
};

static inline struct dcomp_texture *impl_from_IDCompositionTexture(IDCompositionTexture *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_texture, IDCompositionTexture_iface);
}

static const IDCompositionTextureVtbl dcomp_texture_vtbl;

/* Type check for SetContent: only objects created by our
 * CreateCompositionTexture carry dcomp_texture_vtbl. */
static struct dcomp_texture *unsafe_impl_from_IDCompositionTexture(IDCompositionTexture *iface)
{
    if (!iface || iface->lpVtbl != &dcomp_texture_vtbl)
        return NULL;
    return impl_from_IDCompositionTexture(iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_texture_QueryInterface(IDCompositionTexture *iface,
        REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionTexture))
    {
        *out = iface;
        IDCompositionTexture_AddRef(iface);
        return S_OK;
    }

    FIXME("unsupported iid %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dcomp_texture_AddRef(IDCompositionTexture *iface)
{
    struct dcomp_texture *texture = impl_from_IDCompositionTexture(iface);
    ULONG refcount = InterlockedIncrement(&texture->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);
    return refcount;
}

static ULONG STDMETHODCALLTYPE dcomp_texture_Release(IDCompositionTexture *iface)
{
    struct dcomp_texture *texture = impl_from_IDCompositionTexture(iface);
    ULONG refcount = InterlockedDecrement(&texture->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        if (texture->fence)
            ID3D11Fence_Release(&texture->fence->ID3D11Fence_iface);
        if (texture->staging)
            ID3D11Texture2D_Release(texture->staging);
        if (texture->texture)
            ID3D11Texture2D_Release(texture->texture);
        free(texture->bits);
        free(texture);
    }
    return refcount;
}

static HRESULT STDMETHODCALLTYPE dcomp_texture_SetSourceRect(IDCompositionTexture *iface,
        const D2D_RECT_U *source_rect)
{
    struct dcomp_texture *texture = impl_from_IDCompositionTexture(iface);

    if (!source_rect)
        return E_POINTER;

    TRACE("iface %p, source_rect (%u,%u)-(%u,%u).\n", iface,
            source_rect->left, source_rect->top, source_rect->right, source_rect->bottom);

    if (!texture->has_source_rect || memcmp(&texture->source_rect, source_rect, sizeof(*source_rect)))
    {
        texture->source_rect = *source_rect;
        texture->has_source_rect = TRUE;
        texture->last_readback_tick = 0;  /* window changed — force re-readback */
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_texture_SetColorSpace(IDCompositionTexture *iface,
        DXGI_COLOR_SPACE_TYPE color_space)
{
    struct dcomp_texture *texture = impl_from_IDCompositionTexture(iface);

    TRACE("iface %p, color_space %d.\n", iface, color_space);

    texture->color_space = color_space;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_texture_SetAlphaMode(IDCompositionTexture *iface,
        DXGI_ALPHA_MODE alpha_mode)
{
    struct dcomp_texture *texture = impl_from_IDCompositionTexture(iface);

    TRACE("iface %p, alpha_mode %d.\n", iface, alpha_mode);

    texture->alpha_mode = alpha_mode;
    texture->last_readback_tick = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_texture_GetAvailableFence(IDCompositionTexture *iface,
        UINT64 *fence_value, REFIID iid, void **available_fence)
{
    struct dcomp_texture *texture = impl_from_IDCompositionTexture(iface);

    if (!fence_value || !available_fence)
        return E_POINTER;

    if (!texture->fence)
    {
        struct dcomp_fence *fence;

        if (!(fence = calloc(1, sizeof(*fence))))
            return E_OUTOFMEMORY;
        fence->ID3D11Fence_iface.lpVtbl = &dcomp_fence_vtbl;
        fence->refcount = 1;
        if (texture->texture)
            ID3D11Texture2D_GetDevice(texture->texture, &fence->device);
        texture->fence = fence;
    }

    *fence_value = 0;
    return ID3D11Fence_QueryInterface(&texture->fence->ID3D11Fence_iface, iid, available_fence);
}

/* Throttled CPU readback of the wrapped texture (~60 Hz, like
 * last_tree_composite_tick). Fills texture->bits with the BGRA
 * premultiplied source_rect window; both the tree-composite and the
 * serializer path consume the buffer like DComp surface bits. */
static void dcomp_texture_ensure_bits(struct dcomp_texture *texture)
{
    D3D11_TEXTURE2D_DESC staging_desc;
    D3D11_MAPPED_SUBRESOURCE map;
    ID3D11DeviceContext *context;
    ID3D11Device *device;
    UINT x, y, w, h, row;
    BOOL swizzle = FALSE;
    DWORD now = GetTickCount();
    HRESULT hr;

    if (texture->bits && now - texture->last_readback_tick < DCOMP_TREE_FRAME_MS)
        return;
    texture->last_readback_tick = now;

    switch (texture->desc.Format)
    {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            break;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            swizzle = TRUE;
            break;
        default:
            if (!texture->format_warned)
            {
                texture->format_warned = TRUE;
                FIXME("Unsupported texture format %#x — leaf skipped.\n", texture->desc.Format);
            }
            return;
    }

    /* source_rect window, clamped to the texture bounds */
    x = y = 0;
    w = texture->desc.Width;
    h = texture->desc.Height;
    if (texture->has_source_rect)
    {
        x = min(texture->source_rect.left, texture->desc.Width);
        y = min(texture->source_rect.top, texture->desc.Height);
        w = min(texture->source_rect.right, texture->desc.Width) - x;
        h = min(texture->source_rect.bottom, texture->desc.Height) - y;
        if ((int)w <= 0 || (int)h <= 0)
            return;
    }

    if (texture->bits && (texture->bits_width != w || texture->bits_height != h))
    {
        free(texture->bits);
        texture->bits = NULL;
    }
    if (!texture->bits)
    {
        if (!(texture->bits = calloc((SIZE_T)w * h, sizeof(DWORD))))
            return;
        texture->bits_width = w;
        texture->bits_height = h;
    }

    ID3D11Texture2D_GetDevice(texture->texture, &device);
    ID3D11Device_GetImmediateContext(device, &context);

    /* Chromium uses the D3D11 device from multiple threads — make the
     * context calls below safe. (Wine's immediate context only answers
     * ID3D11Multithread, not ID3D10Multithread.) */
    if (!texture->multithread_set)
    {
        ID3D11Multithread *multithread;

        texture->multithread_set = TRUE;
        if (SUCCEEDED(ID3D11DeviceContext_QueryInterface(context,
                &IID_ID3D11Multithread, (void **)&multithread)))
        {
            ID3D11Multithread_SetMultithreadProtected(multithread, TRUE);
            ID3D11Multithread_Release(multithread);
        }
        else
            WARN("No ID3D11Multithread on the immediate context.\n");
    }

    if (!texture->staging)
    {
        staging_desc = texture->desc;
        staging_desc.Usage = D3D11_USAGE_STAGING;
        staging_desc.BindFlags = 0;
        staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging_desc.MiscFlags = 0;
        if (FAILED(hr = ID3D11Device_CreateTexture2D(device, &staging_desc, NULL, &texture->staging)))
        {
            WARN("Failed to create staging texture, hr %#lx.\n", hr);
            goto done;
        }
    }

    ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)texture->staging,
            (ID3D11Resource *)texture->texture);

    if (FAILED(hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)texture->staging, 0,
            D3D11_MAP_READ, 0, &map)))
    {
        WARN("Failed to map staging texture, hr %#lx.\n", hr);
        goto done;
    }

    for (row = 0; row < h; row++)
    {
        const DWORD *src = (const DWORD *)((const BYTE *)map.pData + (SIZE_T)(y + row) * map.RowPitch) + x;
        DWORD *dst = texture->bits + (SIZE_T)row * w;
        UINT i;

        if (!swizzle && texture->alpha_mode != DXGI_ALPHA_MODE_IGNORE)
        {
            memcpy(dst, src, (SIZE_T)w * sizeof(DWORD));
            continue;
        }
        for (i = 0; i < w; i++)
        {
            DWORD s = src[i];

            if (swizzle)  /* RGBA -> BGRA */
                s = (s & 0xff00ff00) | ((s & 0x00ff0000) >> 16) | ((s & 0x000000ff) << 16);
            if (texture->alpha_mode == DXGI_ALPHA_MODE_IGNORE)
                s |= 0xff000000;
            dst[i] = s;
        }
    }

    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)texture->staging, 0);

done:
    ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
}

static const IDCompositionTextureVtbl dcomp_texture_vtbl =
{
    dcomp_texture_QueryInterface,
    dcomp_texture_AddRef,
    dcomp_texture_Release,
    dcomp_texture_SetSourceRect,
    dcomp_texture_SetColorSpace,
    dcomp_texture_SetAlphaMode,
    dcomp_texture_GetAvailableFence,
};

/* Dynamic texture (issue 95): a visual-content indirection whose current
 * composition texture is swapped per frame via SetTexture (Chromium's
 * Runtime-150 delegated-compositing path enters here right after
 * CheckCompositionTextureSupport). */
struct dcomp_dynamic_texture
{
    IDCompositionDynamicTexture IDCompositionDynamicTexture_iface;
    LONG refcount;
    struct dcomp_texture *texture;    /* current content, holds an interface ref */
};

static inline struct dcomp_dynamic_texture *impl_from_IDCompositionDynamicTexture(IDCompositionDynamicTexture *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_dynamic_texture, IDCompositionDynamicTexture_iface);
}

static const IDCompositionDynamicTextureVtbl dcomp_dynamic_texture_vtbl;

/* Type check for SetContent, like unsafe_impl_from_IDCompositionTexture. */
static struct dcomp_dynamic_texture *unsafe_impl_from_IDCompositionDynamicTexture(IDCompositionDynamicTexture *iface)
{
    if (!iface || iface->lpVtbl != &dcomp_dynamic_texture_vtbl)
        return NULL;
    return impl_from_IDCompositionDynamicTexture(iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_dynamic_texture_QueryInterface(IDCompositionDynamicTexture *iface,
        REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionDynamicTexture))
    {
        *out = iface;
        IDCompositionDynamicTexture_AddRef(iface);
        return S_OK;
    }

    FIXME("unsupported iid %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dcomp_dynamic_texture_AddRef(IDCompositionDynamicTexture *iface)
{
    struct dcomp_dynamic_texture *dynamic = impl_from_IDCompositionDynamicTexture(iface);
    ULONG refcount = InterlockedIncrement(&dynamic->refcount);

    TRACE("%p increasing refcount to %lu.\n", dynamic, refcount);
    return refcount;
}

static ULONG STDMETHODCALLTYPE dcomp_dynamic_texture_Release(IDCompositionDynamicTexture *iface)
{
    struct dcomp_dynamic_texture *dynamic = impl_from_IDCompositionDynamicTexture(iface);
    ULONG refcount = InterlockedDecrement(&dynamic->refcount);

    TRACE("%p decreasing refcount to %lu.\n", dynamic, refcount);
    if (!refcount)
    {
        if (dynamic->texture)
            IDCompositionTexture_Release(&dynamic->texture->IDCompositionTexture_iface);
        free(dynamic);
    }
    return refcount;
}

static HRESULT dcomp_dynamic_texture_set_texture(struct dcomp_dynamic_texture *dynamic,
        IDCompositionTexture *texture, ULONG_PTR rect_count)
{
    struct dcomp_texture *impl = NULL;

    if (texture && !(impl = unsafe_impl_from_IDCompositionTexture(texture)))
    {
        FIXME("texture %p is not one of our composition textures.\n", texture);
        return E_INVALIDARG;
    }

    if (impl)
        IDCompositionTexture_AddRef(texture);
    if (dynamic->texture)
        IDCompositionTexture_Release(&dynamic->texture->IDCompositionTexture_iface);
    dynamic->texture = impl;

    /* Force a fresh readback so the next tree composite shows this frame. */
    if (impl)
        impl->last_readback_tick = 0;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_dynamic_texture_SetTexture(IDCompositionDynamicTexture *iface,
        IDCompositionTexture *texture, const D2D_RECT_L *rects, ULONG_PTR rect_count)
{
    struct dcomp_dynamic_texture *dynamic = impl_from_IDCompositionDynamicTexture(iface);

    TRACE("iface %p, texture %p, rects %p, rect_count %Iu.\n", iface, texture, rects, rect_count);

    /* The dirty rects are an optimization only; the readback compositor
     * refreshes the full texture. */
    return dcomp_dynamic_texture_set_texture(dynamic, texture, rect_count);
}

static HRESULT STDMETHODCALLTYPE dcomp_dynamic_texture_SetTextureFull(IDCompositionDynamicTexture *iface,
        IDCompositionTexture *texture)
{
    struct dcomp_dynamic_texture *dynamic = impl_from_IDCompositionDynamicTexture(iface);

    TRACE("iface %p, texture %p.\n", iface, texture);

    return dcomp_dynamic_texture_set_texture(dynamic, texture, 0);
}

static const IDCompositionDynamicTextureVtbl dcomp_dynamic_texture_vtbl =
{
    dcomp_dynamic_texture_QueryInterface,
    dcomp_dynamic_texture_AddRef,
    dcomp_dynamic_texture_Release,
    dcomp_dynamic_texture_SetTexture,
    dcomp_dynamic_texture_SetTextureFull,
};

/* =====================================================================
 * IDCompositionEffectGroup
 *
 * The object exists so that an application which does not check the result of
 * CreateEffectGroup has something to hold, but the effects are not applied.
 * SetEffect refuses the group for the same reason: an application told that
 * the effect is in place stops drawing the affected content itself.
 * ===================================================================== */

struct dcomp_effect_group
{
    IDCompositionEffectGroup IDCompositionEffectGroup_iface;
    LONG refcount;
};

static inline struct dcomp_effect_group *impl_from_IDCompositionEffectGroup(
        IDCompositionEffectGroup *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_effect_group, IDCompositionEffectGroup_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_effect_group_QueryInterface(
        IDCompositionEffectGroup *iface, REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionEffect)
            || IsEqualGUID(iid, &IID_IDCompositionEffectGroup))
    {
        *out = iface;
        IDCompositionEffectGroup_AddRef(iface);
        return S_OK;
    }

    FIXME("unsupported iid %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dcomp_effect_group_AddRef(IDCompositionEffectGroup *iface)
{
    struct dcomp_effect_group *group = impl_from_IDCompositionEffectGroup(iface);
    ULONG refcount = InterlockedIncrement(&group->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);
    return refcount;
}

static ULONG STDMETHODCALLTYPE dcomp_effect_group_Release(IDCompositionEffectGroup *iface)
{
    struct dcomp_effect_group *group = impl_from_IDCompositionEffectGroup(iface);
    ULONG refcount = InterlockedDecrement(&group->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);

    if (!refcount)
        free(group);
    return refcount;
}

static HRESULT STDMETHODCALLTYPE dcomp_effect_group_SetOpacityAnimation(
        IDCompositionEffectGroup *iface, IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p: not applied.\n", iface, animation);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_effect_group_SetOpacity(
        IDCompositionEffectGroup *iface, float opacity)
{
    FIXME("iface %p, opacity %f: not applied.\n", iface, opacity);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_effect_group_SetTransform3D(
        IDCompositionEffectGroup *iface, IDCompositionTransform3D *transform)
{
    FIXME("iface %p, transform %p: not applied.\n", iface, transform);
    return S_OK;
}

static const IDCompositionEffectGroupVtbl dcomp_effect_group_vtbl =
{
    dcomp_effect_group_QueryInterface,
    dcomp_effect_group_AddRef,
    dcomp_effect_group_Release,
    dcomp_effect_group_SetOpacityAnimation,
    dcomp_effect_group_SetOpacity,
    dcomp_effect_group_SetTransform3D,
};

static HRESULT dcomp_effect_group_create(IDCompositionEffectGroup **out)
{
    struct dcomp_effect_group *group;

    if (!out)
        return E_INVALIDARG;

    *out = NULL;
    if (!(group = calloc(1, sizeof(*group))))
        return E_OUTOFMEMORY;

    group->IDCompositionEffectGroup_iface.lpVtbl = &dcomp_effect_group_vtbl;
    group->refcount = 1;

    *out = &group->IDCompositionEffectGroup_iface;
    return S_OK;
}

struct dcomp_visual
{
    IDCompositionVisual IDCompositionVisual_iface;
    LONG refcount;
    IUnknown *content;
    struct dcomp_surface *surface_content; /* non-NULL if content is a DComp surface */
    struct dcomp_texture *texture_content; /* non-NULL if content is a composition texture */
    struct dcomp_dynamic_texture *dynamic_content; /* non-NULL if content is a dynamic texture */
    HWND target_hwnd;
    /* Visual tree */
    float offset_x;
    float offset_y;
    struct dcomp_visual *parent;
    struct dcomp_visual *children;       /* head of child list (first = back) */
    struct dcomp_visual *next_sibling;   /* doubly-linked sibling list */
    struct dcomp_visual *prev_sibling;
};

static inline struct dcomp_visual *impl_from_IDCompositionVisual(IDCompositionVisual *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_visual, IDCompositionVisual_iface);
}

/* The composition texture a visual currently shows: direct texture content,
 * or the dynamic texture's current frame (issue 95). */
static inline struct dcomp_texture *dcomp_visual_effective_texture(struct dcomp_visual *visual)
{
    if (visual->texture_content)
        return visual->texture_content;
    if (visual->dynamic_content)
        return visual->dynamic_content->texture;
    return NULL;
}

static void dcomp_visual_try_reparent(struct dcomp_visual *visual);
static void dcomp_commit_visual_tree(HWND target_hwnd, struct dcomp_visual *root);

/* Visual tree linked-list helpers */
static void dcomp_visual_unlink(struct dcomp_visual *child)
{
    if (!child->parent)
        return;

    if (child->prev_sibling)
        child->prev_sibling->next_sibling = child->next_sibling;
    else
        child->parent->children = child->next_sibling;

    if (child->next_sibling)
        child->next_sibling->prev_sibling = child->prev_sibling;

    child->parent = NULL;
    child->prev_sibling = NULL;
    child->next_sibling = NULL;
}

static void dcomp_visual_append(struct dcomp_visual *parent, struct dcomp_visual *child)
{
    struct dcomp_visual *last = parent->children;

    child->parent = parent;
    child->prev_sibling = NULL;
    child->next_sibling = NULL;

    if (!last)
    {
        parent->children = child;
        return;
    }

    while (last->next_sibling)
        last = last->next_sibling;

    last->next_sibling = child;
    child->prev_sibling = last;
}

static void dcomp_visual_prepend(struct dcomp_visual *parent, struct dcomp_visual *child)
{
    child->parent = parent;
    child->prev_sibling = NULL;
    child->next_sibling = parent->children;

    if (parent->children)
        parent->children->prev_sibling = child;

    parent->children = child;
}

static void dcomp_visual_insert_after(struct dcomp_visual *ref, struct dcomp_visual *child)
{
    child->parent = ref->parent;
    child->prev_sibling = ref;
    child->next_sibling = ref->next_sibling;

    if (ref->next_sibling)
        ref->next_sibling->prev_sibling = child;

    ref->next_sibling = child;
}

static void dcomp_visual_insert_before(struct dcomp_visual *ref, struct dcomp_visual *child)
{
    child->parent = ref->parent;
    child->next_sibling = ref;
    child->prev_sibling = ref->prev_sibling;

    if (ref->prev_sibling)
        ref->prev_sibling->next_sibling = child;
    else if (ref->parent)
        ref->parent->children = child;

    ref->prev_sibling = child;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_QueryInterface(IDCompositionVisual *iface,
        REFIID iid, void **out)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionVisual))
    {
        *out = &visual->IDCompositionVisual_iface;
        IDCompositionVisual_AddRef(*out);
        return S_OK;
    }

    FIXME("unsupported iid %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dcomp_visual_AddRef(IDCompositionVisual *iface)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual(iface);
    ULONG refcount = InterlockedIncrement(&visual->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);
    return refcount;
}

static ULONG STDMETHODCALLTYPE dcomp_visual_Release(IDCompositionVisual *iface)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual(iface);
    ULONG refcount = InterlockedDecrement(&visual->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        /* Unlink from parent if still in a tree */
        if (visual->parent)
            dcomp_visual_unlink(visual);

        /* Release all children */
        {
            struct dcomp_visual *child = visual->children;
            while (child)
            {
                struct dcomp_visual *next = child->next_sibling;
                child->parent = NULL;
                child->prev_sibling = NULL;
                child->next_sibling = NULL;
                IDCompositionVisual_Release(&child->IDCompositionVisual_iface);
                child = next;
            }
            visual->children = NULL;
        }

        if (visual->content)
            IUnknown_Release(visual->content);
        free(visual);
    }
    return refcount;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetOffsetXAnimation(IDCompositionVisual *iface,
        IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetOffsetX(IDCompositionVisual *iface, float offset_x)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual(iface);

    TRACE("iface %p, offset_x %.8e.\n", iface, offset_x);
    visual->offset_x = offset_x;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetOffsetYAnimation(IDCompositionVisual *iface,
        IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetOffsetY(IDCompositionVisual *iface, float offset_y)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual(iface);

    TRACE("iface %p, offset_y %.8e.\n", iface, offset_y);
    visual->offset_y = offset_y;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetTransformObject(IDCompositionVisual *iface,
        IDCompositionTransform *transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetTransform(IDCompositionVisual *iface,
        const D2D_MATRIX_3X2_F *matrix)
{
    FIXME("iface %p, matrix %p stub!\n", iface, matrix);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetTransformParent(IDCompositionVisual *iface,
        IDCompositionVisual *visual)
{
    FIXME("iface %p, visual %p stub!\n", iface, visual);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetEffect(IDCompositionVisual *iface,
        IDCompositionEffect *effect)
{
    FIXME("iface %p, effect %p: effects are not applied.\n", iface, effect);

    /* Refusing the effect is deliberate: told that it took, the application
     * hands the affected content to the compositor and stops drawing it. */
    return effect ? E_NOTIMPL : S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetBitmapInterpolationMode(IDCompositionVisual *iface,
        enum DCOMPOSITION_BITMAP_INTERPOLATION_MODE interpolation_mode)
{
    FIXME("iface %p, mode %d stub!\n", iface, interpolation_mode);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetBorderMode(IDCompositionVisual *iface,
        enum DCOMPOSITION_BORDER_MODE border_mode)
{
    FIXME("iface %p, mode %d stub!\n", iface, border_mode);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetClipObject(IDCompositionVisual *iface,
        IDCompositionClip *clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetClip(IDCompositionVisual *iface,
        const D2D_RECT_F *rect)
{
    FIXME("iface %p, rect %p stub!\n", iface, rect);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetContent(IDCompositionVisual *iface,
        IUnknown *content)
{
    struct dcomp_visual *visual = impl_from_IDCompositionVisual(iface);
    IDCompositionSurface *surface_iface = NULL;

    FIXME("iface %p, content %p (visual: target_hwnd %p).\n",
            iface, content, visual->target_hwnd);

    if (visual->content)
        IUnknown_Release(visual->content);

    visual->content = content;
    visual->surface_content = NULL;
    visual->texture_content = NULL;
    visual->dynamic_content = NULL;
    if (content)
    {
        IDCompositionDynamicTexture *dynamic_iface;
        IDCompositionTexture *texture_iface;

        IUnknown_AddRef(content);

        /* Check if the content is one of our DComp surfaces */
        if (SUCCEEDED(IUnknown_QueryInterface(content, &IID_IDCompositionSurface,
                (void **)&surface_iface)))
        {
            visual->surface_content = impl_from_IDCompositionSurface(surface_iface);
            IDCompositionSurface_Release(surface_iface);
            FIXME("Visual %p: content is DComp surface %p (%ux%u).\n",
                    visual, visual->surface_content,
                    visual->surface_content->width, visual->surface_content->height);
        }
        /* ... or one of our composition textures (issue 90) */
        else if (SUCCEEDED(IUnknown_QueryInterface(content, &IID_IDCompositionTexture,
                (void **)&texture_iface)))
        {
            visual->texture_content = unsafe_impl_from_IDCompositionTexture(texture_iface);
            IDCompositionTexture_Release(texture_iface);
        }
        /* ... or a dynamic texture (issue 95) */
        else if (SUCCEEDED(IUnknown_QueryInterface(content, &IID_IDCompositionDynamicTexture,
                (void **)&dynamic_iface)))
        {
            visual->dynamic_content = unsafe_impl_from_IDCompositionDynamicTexture(dynamic_iface);
            IDCompositionDynamicTexture_Release(dynamic_iface);
        }
    }

    /* Only redirect root visuals (no parent) to target_hwnd.
     * Child visuals keep their hidden comp window; the root's
     * Present composites them via Porter-Duff Over.
     * Surface content does not need swapchain reparenting. */
    if (visual->surface_content || visual->texture_content || visual->dynamic_content)
    {
        /* Surface/texture content: no swapchain to reparent. The composition
         * integration happens through direct bits in commit_visual_tree. */
    }
    else if (!visual->parent)
        dcomp_visual_try_reparent(visual);
    else
        dcomp_send_child_mode(visual->content);

    /* Chromium swaps swapchain content on an existing leaf visual per frame.
     * Re-serialize the tree so the target's child props track the current
     * comp window (the AddVisual hook only fires on tree changes). */
    {
        struct dcomp_visual *root = visual;

        while (root->parent)
            root = root->parent;
        if (root->target_hwnd)
            dcomp_commit_visual_tree(root->target_hwnd, root);
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_AddVisual(IDCompositionVisual *iface,
        IDCompositionVisual *visual, BOOL insert_above, IDCompositionVisual *reference_visual)
{
    struct dcomp_visual *parent = impl_from_IDCompositionVisual(iface);
    struct dcomp_visual *child = impl_from_IDCompositionVisual(visual);
    struct dcomp_visual *ref = reference_visual
            ? impl_from_IDCompositionVisual(reference_visual) : NULL;

    FIXME("iface %p, visual %p, insert_above %d, reference %p.\n",
            iface, visual, insert_above, reference_visual);

    if (!visual)
        return E_INVALIDARG;

    /* Remove from old parent if already in a tree */
    if (child->parent)
    {
        dcomp_visual_unlink(child);
        IDCompositionVisual_Release(visual);
    }

    /* Inherit target_hwnd from root of tree */
    if (parent->target_hwnd)
        child->target_hwnd = parent->target_hwnd;

    if (!ref)
    {
        /* No reference: insert_above=TRUE → on top (end), FALSE → at bottom (start) */
        if (insert_above)
            dcomp_visual_append(parent, child);
        else
            dcomp_visual_prepend(parent, child);
    }
    else
    {
        if (insert_above)
            dcomp_visual_insert_after(ref, child);
        else
            dcomp_visual_insert_before(ref, child);
    }

    IDCompositionVisual_AddRef(visual);

    /* If child already has content, enable child mode on its swapchain */
    if (child->content)
        dcomp_send_child_mode(child->content);

    /* Serialize tree info for the root's target window */
    {
        struct dcomp_visual *root = parent;
        while (root->parent)
            root = root->parent;
        if (root->target_hwnd)
            dcomp_commit_visual_tree(root->target_hwnd, root);
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_RemoveVisual(IDCompositionVisual *iface,
        IDCompositionVisual *visual)
{
    struct dcomp_visual *child = impl_from_IDCompositionVisual(visual);

    FIXME("iface %p, visual %p.\n", iface, visual);

    if (!visual)
        return E_INVALIDARG;

    if (child->parent)
    {
        struct dcomp_visual *root = child->parent;
        dcomp_visual_unlink(child);
        IDCompositionVisual_Release(visual);

        /* Re-serialize tree after removal */
        while (root->parent)
            root = root->parent;
        if (root->target_hwnd)
            dcomp_commit_visual_tree(root->target_hwnd, root);
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_RemoveAllVisuals(IDCompositionVisual *iface)
{
    struct dcomp_visual *parent = impl_from_IDCompositionVisual(iface);
    struct dcomp_visual *child = parent->children;

    FIXME("iface %p.\n", iface);

    while (child)
    {
        struct dcomp_visual *next = child->next_sibling;
        child->parent = NULL;
        child->prev_sibling = NULL;
        child->next_sibling = NULL;
        IDCompositionVisual_Release(&child->IDCompositionVisual_iface);
        child = next;
    }
    parent->children = NULL;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual_SetCompositeMode(IDCompositionVisual *iface,
        enum DCOMPOSITION_COMPOSITE_MODE composite_mode)
{
    FIXME("iface %p, mode %d stub!\n", iface, composite_mode);
    return S_OK;
}

static const IDCompositionVisualVtbl dcomp_visual_vtbl =
{
    dcomp_visual_QueryInterface,
    dcomp_visual_AddRef,
    dcomp_visual_Release,
    dcomp_visual_SetOffsetXAnimation,
    dcomp_visual_SetOffsetX,
    dcomp_visual_SetOffsetYAnimation,
    dcomp_visual_SetOffsetY,
    dcomp_visual_SetTransformObject,
    dcomp_visual_SetTransform,
    dcomp_visual_SetTransformParent,
    dcomp_visual_SetEffect,
    dcomp_visual_SetBitmapInterpolationMode,
    dcomp_visual_SetBorderMode,
    dcomp_visual_SetClipObject,
    dcomp_visual_SetClip,
    dcomp_visual_SetContent,
    dcomp_visual_AddVisual,
    dcomp_visual_RemoveVisual,
    dcomp_visual_RemoveAllVisuals,
    dcomp_visual_SetCompositeMode,
};

/* IDCompositionVisual2 extends IDCompositionVisual with two setters. Provide a
 * real v2 vtable so objects returned through IDCompositionVisual2* dispatch
 * those methods correctly instead of running past the 20-slot v1 vtable
 * (crash). The first 20 slots are layout-identical to the v1 vtable and reuse
 * its methods (the IDCompositionVisual2* and IDCompositionVisual* this-pointers
 * are binary-identical), so they are cast in. */
static HRESULT STDMETHODCALLTYPE dcomp_visual2_SetOpacityMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_OPACITY_MODE opacity_mode)
{
    FIXME("iface %p, opacity_mode %d - stub.\n", iface, opacity_mode);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_visual2_SetBackFaceVisibility(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_BACKFACE_VISIBILITY visibility)
{
    FIXME("iface %p, visibility %d - stub.\n", iface, visibility);
    return S_OK;
}

static const IDCompositionVisual2Vtbl dcomp_visual2_vtbl =
{
    (void *)dcomp_visual_QueryInterface,
    (void *)dcomp_visual_AddRef,
    (void *)dcomp_visual_Release,
    (void *)dcomp_visual_SetOffsetXAnimation,
    (void *)dcomp_visual_SetOffsetX,
    (void *)dcomp_visual_SetOffsetYAnimation,
    (void *)dcomp_visual_SetOffsetY,
    (void *)dcomp_visual_SetTransformObject,
    (void *)dcomp_visual_SetTransform,
    (void *)dcomp_visual_SetTransformParent,
    (void *)dcomp_visual_SetEffect,
    (void *)dcomp_visual_SetBitmapInterpolationMode,
    (void *)dcomp_visual_SetBorderMode,
    (void *)dcomp_visual_SetClipObject,
    (void *)dcomp_visual_SetClip,
    (void *)dcomp_visual_SetContent,
    (void *)dcomp_visual_AddVisual,
    (void *)dcomp_visual_RemoveVisual,
    (void *)dcomp_visual_RemoveAllVisuals,
    (void *)dcomp_visual_SetCompositeMode,
    dcomp_visual2_SetOpacityMode,
    dcomp_visual2_SetBackFaceVisibility,
};

#define WM_WINE_DCOMP_SET_TARGET (WM_USER + 0x100)

static void dcomp_visual_try_reparent(struct dcomp_visual *visual)
{
    WCHAR prop_name[64];
    HWND comp_wnd;

    if (!visual->content || !visual->target_hwnd)
    {
        FIXME("Reparent skipped: content %p, target_hwnd %p.\n",
                visual->content, visual->target_hwnd);
        return;
    }

    swprintf(prop_name, ARRAY_SIZE(prop_name),
            WINE_DCOMP_WND_PROP_FMT, GetCurrentProcessId(), (UINT_PTR)visual->content);
    comp_wnd = (HWND)GetPropW(GetDesktopWindow(), prop_name);

    if (!comp_wnd)
    {
        FIXME("Composition window NOT FOUND for content %p (prop: %s).\n",
                visual->content, debugstr_w(prop_name));
        return;
    }

    TRACE("Switching swapchain window from %p to target %p.\n",
            comp_wnd, visual->target_hwnd);

    /* Tell dxgi to switch the swapchain's rendering target to target_hwnd.
     * This avoids SetParent which destroys the X11 window and breaks GL rendering.
     * The wndproc in dxgi/factory.c handles this message by calling
     * wined3d_swapchain_set_window, which triggers lazy GL context rebinding. */
    SendMessageW(comp_wnd, WM_WINE_DCOMP_SET_TARGET, 0, (LPARAM)visual->target_hwnd);

    /* DWM composes swapchain content into the target's background — always
     * below child windows. Our comp window is a real child window and would
     * otherwise land on top of the sibling stack, covering e.g. the WebView2/
     * Chromium child window with the loader's placeholder (issue 88: grey box
     * over the EPROM login UI, z-fight on every resize). Single-child plugin
     * targets are unaffected by HWND_BOTTOM. */
    SetWindowPos(comp_wnd, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    FIXME("Requested swapchain window switch from %p to target %p.\n",
            comp_wnd, visual->target_hwnd);
}

/* =====================================================================
 * IDCompositionTarget
 * ===================================================================== */

/* Property name for storing target pointer on HWND (Phase 5 subclass) */
static const WCHAR dcomp_target_prop[] = L"__wine_dcomp_target";
/* Set (in-process targets only) while a subclassed wndproc is in place that
 * will handle WM_WINE_DCOMP_PRESENT_FLUSH.  dxgi reads it after each present
 * to decide whether to post the flush message at all -- foreign-process
 * targets have no subclass of ours, so they are never signalled. */
static const WCHAR dcomp_present_flush_prop[] = L"__wine_dcomp_present_flush";

/* The number of leaves dcomp_serialize_visual_leaves() writes to the target
 * window.  Kept as a name so the commit log can say which limit it hit;
 * wined3d has its own copy of the number in swapchain_composite_children(). */
#define DCOMP_MAX_SERIALIZED_LEAVES 16

/* The application's own wndproc, stashed on the window when it is first
 * subclassed so later targets can restore the chain (issue 98). */
static const WCHAR dcomp_real_wndproc_prop[] = L"__wine_dcomp_real_wndproc";
/* The composition wndproc currently installed on the window, whichever module
 * put it there.  dxgi subclasses composition targets too, and its procedure
 * lives in another DLL, so comparing function pointers only ever recognises
 * our own.  Both modules publish what they installed here and consult it
 * before chaining, so neither builds a chain that runs through the other and
 * back into itself (issue 101). */
static const WCHAR dcomp_subclass_proc_prop[] = L"__wine_dcomp_subclass_proc";

static LRESULT CALLBACK dcomp_target_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

struct dcomp_target
{
    IDCompositionTarget IDCompositionTarget_iface;
    LONG refcount;
    HWND hwnd;
    struct dcomp_visual *root_visual;
    struct dcomp_device *device;          /* back-pointer for target list management */
    struct dcomp_target *next_target;     /* singly-linked list in dcomp_device */
    /* Presentation state for surface content → HWND blitting */
    HDC comp_dc;
    HBITMAP comp_bitmap;
    DWORD *comp_bits;                     /* DIB section pixel buffer */
    UINT comp_width, comp_height;
    BOOL comp_needs_full_present;         /* force a full BitBlt on first present after DIB (re)create */
    BOOL comp_backdrop_valid;             /* comp_bits holds a backdrop captured successfully at the current size (issue 116) */
    LONGLONG last_present_qpc;            /* QPC of last actual present — drives ~60 Hz coalescing (issue 56) */
    BOOL foreign;                         /* target hwnd belongs to another process — no subclass, hook-driven compositing (issue 88) */
    DWORD last_tree_composite_tick;       /* GetTickCount of last hook-driven tree composite (~60 Hz rate limit) */
    /* Sticky: this target's tree has carried at least one content leaf at some
     * point (issue 184).  Latched, never cleared — a rootless tree may be
     * momentarily empty between two generations, and taking over the window
     * only while it actually has content would hand those frames back to the
     * application mid-flight.  Never latched at all means the tree carries
     * nothing and this target must keep its hands off the window. */
    BOOL tree_had_content;
    /* Client-space union of every leaf rectangle this target's tree has ever
     * covered (issue 187).  Kept as the WINE_DCOMP_COVER_FRAME=0 measurement
     * for the covers_window threshold: by default that threshold asks the
     * CURRENT frame's region instead (issue 205), because this union grows
     * with every position a moving leaf has ever had, and a playhead 1 px
     * wide crosses the 50% mark on accumulation alone after ~1200 columns --
     * taking the window over from a tree that covers a sliver of every
     * frame.  NULL until the first composite. */
    HRGN covered_rgn;
    /* The leaf region of the current frame (unresolved fallback included) has
     * reached DCOMP_MIN_COVER_PERCENT of the client area at least once, i.e.
     * our composition can plausibly be said to BE what this window shows.
     * Below that we keep off the window entirely: no blit, no WM_PAINT, not
     * even the 60 Hz read-back (issue 187).  Latched -- the verdict is, the
     * measurement is not (issue 205). */
    BOOL covers_window;
    /* Below that threshold we still deliver what the tree covers, just without
     * claiming the window (issue 190).  These two are that mode's whole state:
     * the leaf region of the CURRENT frame -- covered_rgn latches and a moving
     * leaf would grow it into a band -- and the region the last delivery
     * actually painted, so what we stop covering can be handed back. */
    HRGN frame_rgn;
    HRGN delivered_rgn;
    /* Save-under for the region delivery (issue 196): a copy of the window
     * content as it was BEFORE our blit, for exactly the area the last
     * delivery painted (saved_rgn).  Until now vacated pixels were the
     * application's problem -- InvalidateRgn, one attempt, no receipt -- and
     * a compositing application paints nothing there, because the moving
     * leaf lives in the tree precisely so it does not have to.  On Windows
     * the DWM clears the old position while compositing; here it had already
     * been written into the window and stood there for good.  So the cleanup
     * is our own idempotent action now: restore the saved backdrop, and keep
     * the invalidation next to it as a correction path for when the
     * application did change something underneath us.  WINE_DCOMP_SAVE_UNDER=0
     * returns to delegating the whole cleanup. */
    HDC     save_dc;
    HBITMAP save_bitmap;
    DWORD  *save_bits;
    UINT    save_width, save_height;
    HRGN    saved_rgn;   /* region for which save_dc holds a valid backdrop */
    /* That delivery runs in a thread of ours instead of the application's GUI
     * thread (issue 190, WINE_DCOMP_DELIVER_THREAD).  deliver_rgn is the
     * thread's own copy of frame_rgn, taken under the device lock, so the tree
     * walk can keep rewriting the original while a delivery is in flight. */
    HANDLE deliver_thread;
    HANDLE deliver_event;
    LONG deliver_stop;
    HRGN deliver_rgn;
    /* The leaves as a layer with alpha, so the present path can draw them into
     * the frame instead of blitting them onto the window afterwards (issue
     * 206).  The contract, the locking and the lifetime rules are in
     * include/wine/dcomp_layer.h.
     *
     * `layer` is what wined3d reads and outlives this target; the two pixel
     * buffers and their dirty boxes are ours alone.  We composite into
     * layer_buf[layer_back] and publish that, so a reader is never handed a
     * half-written frame. */
    struct wine_dcomp_layer *layer;
    DWORD *layer_buf[2];
    RECT layer_dirty[2][WINE_DCOMP_LAYER_MAX_RECTS];
    unsigned int layer_dirty_n[2];
    unsigned int layer_back;
    unsigned int layer_width, layer_height;
    BOOL layer_live;                      /* a non-empty box is published right now */
    /* Is the sink actually drawing?  layer_drawn_seen is the compositor's draw
     * count at the last delivery, layer_unseen counts deliveries since it last
     * moved, layer_route_off means we gave up and went back to blitting, and
     * layer_sink_seen is the sink count we gave up against -- a change to it
     * means the set of swapchains on this window changed and it is worth
     * another try. */
    LONG layer_drawn_seen;
    unsigned int layer_unseen;
    unsigned int layer_retry;
    ULONG_PTR layer_sink_seen;
    BOOL layer_route_off;
    /* Unchanged-content gate (issue 99): hash over all content-leaf sources
     * of the current walk vs. the hash of the last frame that actually
     * reached the window.  The tree timer must only push NEW leaf content —
     * re-blitting an unchanged composition either does nothing or wallpapers
     * over the host's own painting (tab-switch race).  Trees with unhashable
     * leaves (surface/texture content) are never skipped. */
    DWORD walk_leaf_hash;
    BOOL walk_leaf_hash_valid;
    DWORD last_blit_leaf_hash;
    BOOL last_blit_leaf_valid;
    RECT last_blit_clip_rc;               /* clip box of the last blit that really painted */
    HWND last_foreground;                 /* foreground window at the last timer composite */
    /* Target-side verification: the leaf hash proves our source is unchanged,
     * never that the window still shows what we delivered (issue 107). */
    DWORD last_delivered_hash;            /* sampled hash of the last frame we blitted */
    BOOL last_delivered_valid;
    DWORD target_diverged_tick;           /* GetTickCount when the window stopped matching, 0 = matches */
    /* Host-restore bookkeeping (issue 99): hand areas our blits vacated
     * (window hidden or resized) back to the host for repaint. */
    int hide_restore_clip;                /* last present clip verdict (visible->hidden edge) */
    RECT last_blit_win_rect;              /* screen rect at the last delivered blit */
    BOOL last_blit_win_valid;
    RECT last_blit_top_rect;              /* toplevel screen rect at that moment */
    /* WndProc subclass for WM_ERASEBKGND / WM_PAINT protection (Phase 5) */
    WNDPROC orig_wndproc;
};

static inline struct dcomp_target *impl_from_IDCompositionTarget(IDCompositionTarget *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_target, IDCompositionTarget_iface);
}

static void dcomp_target_join_deliver_thread(struct dcomp_target *target);

static HRESULT STDMETHODCALLTYPE dcomp_target_QueryInterface(IDCompositionTarget *iface,
        REFIID iid, void **out)
{
    struct dcomp_target *target = impl_from_IDCompositionTarget(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionTarget))
    {
        *out = &target->IDCompositionTarget_iface;
        IDCompositionTarget_AddRef(*out);
        return S_OK;
    }

    FIXME("unsupported iid %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dcomp_target_AddRef(IDCompositionTarget *iface)
{
    struct dcomp_target *target = impl_from_IDCompositionTarget(iface);
    ULONG refcount = InterlockedIncrement(&target->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);
    return refcount;
}

static ULONG STDMETHODCALLTYPE dcomp_target_Release(IDCompositionTarget *iface)
{
    struct dcomp_target *target = impl_from_IDCompositionTarget(iface);
    ULONG refcount = InterlockedDecrement(&target->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        /* Our delivery thread dereferences this target and takes the device
         * lock, so it has to be gone before either is (issue 190). */
        dcomp_target_join_deliver_thread(target);

        /* Remove from device's target list */
        if (target->device)
            dcomp_device_remove_target(target->device, target);

        /* Stop tree compositing (safe even if the hwnd is already gone) */
        if (target->hwnd)
            KillTimer(target->hwnd, DCOMP_TREE_TIMER);

        /* Remove WndProc subclass (Phase 5).  Restore only while the window still
         * refers to us and our procedure is the installed one: a resize creates a
         * successor target that takes the window over, and dxgi subclasses on top
         * of us, so restoring unconditionally would drop either out of the chain
         * (issue 98, issue 101). */
        if (target->hwnd && IsWindow(target->hwnd))
        {
            BOOL ours = (struct dcomp_target *)GetPropW(target->hwnd, dcomp_target_prop) == target;

            if (ours && target->orig_wndproc
                    && (WNDPROC)GetWindowLongPtrW(target->hwnd, GWLP_WNDPROC) == dcomp_target_wndproc)
            {
                SetWindowLongPtrW(target->hwnd, GWLP_WNDPROC, (LONG_PTR)target->orig_wndproc);
                RemovePropW(target->hwnd, dcomp_subclass_proc_prop);
                FIXME("Removed WndProc subclass from hwnd %p.\n", target->hwnd);
            }
            /* Never leave the back-pointer on a window we are about to free. */
            if (ours)
            {
                RemovePropW(target->hwnd, dcomp_target_prop);
                RemovePropW(target->hwnd, dcomp_present_flush_prop);
            }
        }

        /* Clean up presentation state */
        if (target->covered_rgn)
            DeleteObject(target->covered_rgn);
        if (target->frame_rgn)
            DeleteObject(target->frame_rgn);
        if (target->delivered_rgn)
            DeleteObject(target->delivered_rgn);
        if (target->saved_rgn)
            DeleteObject(target->saved_rgn);
        if (target->save_bitmap)
        {
            SelectObject(target->save_dc, NULL);
            DeleteObject(target->save_bitmap);
        }
        if (target->save_dc)
            DeleteDC(target->save_dc);
        if (target->comp_bitmap)
        {
            SelectObject(target->comp_dc, NULL);
            DeleteObject(target->comp_bitmap);
        }
        if (target->comp_dc)
            DeleteDC(target->comp_dc);
        /* The layer structure outlives us on purpose (dcomp_layer.h); only its
         * pixels go.  Under the exclusive lock, so a present reading it right
         * now finishes first and the next one finds bits NULL. */
        if (target->layer)
        {
            AcquireSRWLockExclusive(&target->layer->lock);
            target->layer->bits = NULL;
            SetRectEmpty(&target->layer->box);
            target->layer->rect_count = 0;
            ReleaseSRWLockExclusive(&target->layer->lock);
        }
        free(target->layer_buf[0]);
        free(target->layer_buf[1]);

        free(target);
    }
    return refcount;
}

static HRESULT STDMETHODCALLTYPE dcomp_target_SetRoot(IDCompositionTarget *iface,
        IDCompositionVisual *visual)
{
    struct dcomp_target *target = impl_from_IDCompositionTarget(iface);
    struct dcomp_visual *visual_impl;

    FIXME("iface %p, visual %p, target hwnd %p.\n", iface, visual, target->hwnd);

    if (!visual)
        return S_OK;

    visual_impl = impl_from_IDCompositionVisual(visual);
    visual_impl->target_hwnd = target->hwnd;
    target->root_visual = visual_impl;

    dcomp_visual_try_reparent(visual_impl);
    return S_OK;
}

static const IDCompositionTargetVtbl dcomp_target_vtbl =
{
    dcomp_target_QueryInterface,
    dcomp_target_AddRef,
    dcomp_target_Release,
    dcomp_target_SetRoot,
};

/* =====================================================================
 * IDCompositionDevice
 * ===================================================================== */

struct dcomp_device
{
    IDCompositionDevice IDCompositionDevice_iface;
    IDCompositionDesktopDevice IDCompositionDesktopDevice_iface;
    IDCompositionDevice5 IDCompositionDevice5_iface;  /* also serves Device3/Device4 QIs */
    LONG refcount;
    IUnknown *rendering_device;   /* IDXGIDevice from DCompositionCreateDevice */
    ID3D11Device *d3d11_device;   /* QI from rendering_device, lazy-init */
    ID2D1Device *d2d1_device;     /* QI from rendering_device if it is ID2D1Device */
    struct dcomp_target *targets; /* singly-linked list of targets for Commit */
    CRITICAL_SECTION cs;          /* protects the targets list (insert/remove/iterate) */
    LONG in_auto_commit;          /* per-device EndDraw auto-commit reentrancy guard */
    LONG commit_pending;          /* re-commit requested while in_auto_commit */
};

static inline struct dcomp_device *impl_from_IDCompositionDevice(IDCompositionDevice *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_device, IDCompositionDevice_iface);
}

static inline struct dcomp_device *impl_from_IDCompositionDesktopDevice(IDCompositionDesktopDevice *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_device, IDCompositionDesktopDevice_iface);
}

static inline struct dcomp_device *impl_from_IDCompositionDevice5(IDCompositionDevice5 *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_device, IDCompositionDevice5_iface);
}

/* Auto-commit from EndDraw with a per-device reentrancy guard.
 * Reentrancy path: Commit → BitBlt → WM_PAINT → BeginDraw/EndDraw → here.
 * The guard is per-device (not a process-global static), so unrelated devices
 * cannot drop each other's commits; atomics keep it safe under concurrent EndDraw. */
static void dcomp_device_auto_commit(IDCompositionDevice *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice(iface);

    if (!InterlockedCompareExchange(&device->in_auto_commit, TRUE, FALSE))
    {
        dcomp_device_Commit(iface);
        while (InterlockedExchange(&device->commit_pending, FALSE))
            dcomp_device_Commit(iface);
        InterlockedExchange(&device->in_auto_commit, FALSE);
    }
    else
    {
        /* reentrant EndDraw: request a re-commit after the outer one finishes */
        InterlockedExchange(&device->commit_pending, TRUE);
    }
}

static HRESULT STDMETHODCALLTYPE dcomp_device_QueryInterface(IDCompositionDevice *iface,
        REFIID iid, void **out)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
            || IsEqualGUID(iid, &IID_IDCompositionDevice))
    {
        *out = &device->IDCompositionDevice_iface;
        IDCompositionDevice_AddRef(*out);
        return S_OK;
    }

    if (IsEqualGUID(iid, &IID_IDCompositionDevice2)
            || IsEqualGUID(iid, &IID_IDCompositionDesktopDevice))
    {
        *out = &device->IDCompositionDesktopDevice_iface;
        IDCompositionDesktopDevice_AddRef(*out);
        return S_OK;
    }

    if (IsEqualGUID(iid, &IID_IDCompositionDevice3)
            || IsEqualGUID(iid, &IID_IDCompositionDevice4)
            || IsEqualGUID(iid, &IID_IDCompositionDevice5))
    {
        *out = &device->IDCompositionDevice5_iface;
        IDCompositionDevice5_AddRef(&device->IDCompositionDevice5_iface);
        return S_OK;
    }

    FIXME("unsupported iid %s.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dcomp_device_AddRef(IDCompositionDevice *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice(iface);
    ULONG refcount = InterlockedIncrement(&device->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);
    return refcount;
}

static ULONG STDMETHODCALLTYPE dcomp_device_Release(IDCompositionDevice *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice(iface);
    ULONG refcount = InterlockedDecrement(&device->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        if (device->d2d1_device)
            ID2D1Device_Release(device->d2d1_device);
        if (device->d3d11_device)
            ID3D11Device_Release(device->d3d11_device);
        if (device->rendering_device)
            IUnknown_Release(device->rendering_device);
        DeleteCriticalSection(&device->cs);
        free(device);
    }

    return refcount;
}

static void dcomp_target_composite_tree(struct dcomp_target *target, BOOL from_timer);
static void dcomp_surface_readback_region(struct dcomp_surface *surface,
        LONG l, LONG t, LONG r, LONG b);

/* What the tree holds, for the commit log -- not what was written.  The counts
 * are deliberately independent of the serialization: only swapchain leaves are
 * serialized at all (see dcomp_serialize_visual_leaves below), and only the
 * first DCOMP_MAX_SERIALIZED_LEAVES of those, so a count of what was written
 * would say nothing about the tree.  __wine_dcomp_child_count carries the
 * written number and cannot tell 40 leaves from exactly 16. */
struct dcomp_leaf_stats
{
    unsigned int surface;       /* DComp surface leaves in the tree */
    unsigned int texture;       /* composition-texture leaves in the tree */
    unsigned int swapchain;     /* composition-swapchain leaves in the tree */
    unsigned int total;         /* content-bearing leaves in the tree, uncapped */
    unsigned int over_limit;    /* swapchain leaves past the serialization limit */
    unsigned int no_comp_wnd;   /* swapchain leaf, composition window not found */
};

/* Depth-first leaf serialization with accumulated offsets. The visual's own
 * offset positions its whole subtree (DComp semantics), so it is added on
 * entry. A content-bearing visual is serialized before its children (children
 * render on top). Container visuals (no content) only pass offsets down.
 *
 * Only swapchain leaves are serialized.  DComp surfaces and composition
 * textures used to be written here as well -- a null window handle plus bits,
 * size and offset -- for a reader that never looked at them: the direct-bits
 * branch in wined3d's swapchain_composite_children() sat behind a continue that
 * dropped every leaf without a window handle, and had done so since it was
 * added (e99905af0dd, 31.03.2026).  A measurement over eight configurations on
 * 01.09.2026 found no application in the field that reaches the combination,
 * so the writes were removed rather than the reader repaired.
 *
 * The consumer of these leaves is the other compositing path:
 * dcomp_target_composite_leaves() walks the visual tree itself, knows all three
 * kinds and does its own texture readback.  It drives every rootless tree, which
 * is where surface and texture leaves actually occur.
 *
 * Every leaf is still counted, by kind and uncapped -- see dcomp_leaf_stats and
 * the report in dcomp_commit_visual_tree().  That count is what is left of the
 * removed code: an application that does build such a tree says so in the log
 * instead of losing its pixels in silence. */
static unsigned int dcomp_serialize_visual_leaves(HWND target_hwnd, struct dcomp_visual *visual,
        int base_x, int base_y, unsigned int idx, struct dcomp_leaf_stats *stats)
{
    WCHAR prop_name[64];
    struct dcomp_visual *child;
    int vx = base_x + (int)visual->offset_x;
    int vy = base_y + (int)visual->offset_y;

    if (visual->surface_content)
    {
        /* Counted, not serialized: composited by dcomp_target_composite_leaves(). */
        ++stats->surface;
        ++stats->total;
    }
    else if (dcomp_visual_effective_texture(visual))
    {
        /* Composition texture (issue 90) or dynamic texture's current frame
         * (issue 95).  Counted, not serialized -- and no dcomp_texture_ensure_bits()
         * here, so the throttled GPU->CPU readback no longer runs for a consumer
         * that does not exist.  The compositing path does its own. */
        ++stats->texture;
        ++stats->total;
    }
    else if (visual->content)
    {
        /* Swapchain content: look up the visual's composition window */
        HWND child_comp_wnd;

        ++stats->swapchain;
        ++stats->total;

        if (idx >= DCOMP_MAX_SERIALIZED_LEAVES)
        {
            /* Past the limit the walk only counts, so the commit log can say how
             * much was dropped.  It keeps descending: the children are counted too. */
            ++stats->over_limit;
        }
        else
        {
            swprintf(prop_name, ARRAY_SIZE(prop_name),
                    WINE_DCOMP_WND_PROP_FMT, GetCurrentProcessId(), (UINT_PTR)visual->content);
            child_comp_wnd = (HWND)GetPropW(GetDesktopWindow(), prop_name);
            if (child_comp_wnd)
            {
                swprintf(prop_name, ARRAY_SIZE(prop_name),
                        L"__wine_dcomp_child_%u_wnd", idx);
                SetPropW(target_hwnd, prop_name, (HANDLE)child_comp_wnd);

                /* Pack offset as two signed 16-bit values */
                if ((short)vx != vx || (short)vy != vy)
                {
                    /* The pack wraps: the reader sees a different, valid-looking
                     * offset and clips the leaf away without a trace.  Noted since
                     * 03/2026, never seen -- the offsets only accumulate parent
                     * offsets -- but a tree deep enough would hit it silently. */
                    static unsigned int wrap_count;

                    if (++wrap_count <= 5 || !(wrap_count % 200))
                        FIXME("Leaf offset (%d,%d) on target %p does not survive the 16-bit "
                                "pack (report #%u): the reader will place the leaf at (%d,%d).\n",
                                vx, vy, target_hwnd, wrap_count, (short)vx, (short)vy);
                }
                swprintf(prop_name, ARRAY_SIZE(prop_name),
                        L"__wine_dcomp_child_%u_offset", idx);
                SetPropW(target_hwnd, prop_name, (HANDLE)(ULONG_PTR)MAKELPARAM(vx, vy));

                ++idx;
            }
            else
            {
                /* No composition window behind the swapchain: the leaf cannot be
                 * addressed and is dropped.  Either dxgi has not created the window
                 * yet or the desktop property was lost. */
                static unsigned int no_wnd_count;

                ++stats->no_comp_wnd;
                if (++no_wnd_count <= 5 || !(no_wnd_count % 200))
                    FIXME("Leaf dropped #%u: no composition window for swapchain content %p "
                            "on visual %p, target %p.\n",
                            no_wnd_count, visual->content, visual, target_hwnd);
            }
        }
    }

    for (child = visual->children; child; child = child->next_sibling)
        idx = dcomp_serialize_visual_leaves(target_hwnd, child, vx, vy, idx, stats);

    return idx;
}

/* What a target last reported about itself.  Two reports must not repeat every
 * commit -- the early exit below, which is the most frequent outcome of all, and
 * a change of operating mode -- so each target remembers its last note.
 *
 * The table is kept in process memory and keyed by the target window rather than
 * held on struct dcomp_target, because the early exit has nothing but the HWND:
 * resolving the target there means a GetPropW, and that is a wineserver round
 * trip in a path that runs once per present.  Unlocked, like the throttle
 * counters next to it; the worst outcome is one duplicated or one missing
 * diagnostic line. */
#define DCOMP_COMMIT_NOTE_SLOTS 16

enum dcomp_commit_note
{
    DCOMP_NOTE_NONE,                /* nothing reported yet, or a free slot */
    DCOMP_NOTE_NO_TARGET,           /* commit without a target window */
    DCOMP_NOTE_NO_ROOT,             /* target has no root visual */
    DCOMP_NOTE_CONTENT_ROOT_ALONE,  /* root carries content but has no children */
    DCOMP_NOTE_EMPTY_ROOT,          /* root has neither content nor children */
    DCOMP_NOTE_ROOTLESS,            /* committed, rootless tree */
    DCOMP_NOTE_CONTENT_ROOT,        /* committed, content-bearing root */
};

static const char *dcomp_commit_note_text(enum dcomp_commit_note note)
{
    switch (note)
    {
        case DCOMP_NOTE_NO_TARGET:          return "no target window";
        case DCOMP_NOTE_NO_ROOT:            return "no root visual";
        case DCOMP_NOTE_CONTENT_ROOT_ALONE: return "content-root without children";
        case DCOMP_NOTE_EMPTY_ROOT:         return "root without content and without children";
        case DCOMP_NOTE_ROOTLESS:           return "rootless";
        case DCOMP_NOTE_CONTENT_ROOT:       return "content-root";
        default:                            return "none";
    }
}

/* Store this target's note and hand back the one it carried before, so callers
 * can report a first sighting and a change but stay quiet on a repeat. */
static enum dcomp_commit_note dcomp_commit_note_set(HWND hwnd, enum dcomp_commit_note note)
{
    static struct { HWND hwnd; enum dcomp_commit_note note; } notes[DCOMP_COMMIT_NOTE_SLOTS];
    static unsigned int next_slot;
    unsigned int i;

    for (i = 0; i < DCOMP_COMMIT_NOTE_SLOTS; ++i)
    {
        if (notes[i].note != DCOMP_NOTE_NONE && notes[i].hwnd == hwnd)
        {
            enum dcomp_commit_note prev = notes[i].note;

            notes[i].note = note;
            return prev;
        }
    }
    for (i = 0; i < DCOMP_COMMIT_NOTE_SLOTS; ++i)
    {
        if (notes[i].note == DCOMP_NOTE_NONE)
            break;
    }
    /* More targets than slots: reuse round-robin.  A reused slot reports its new
     * owner once more than necessary, which is the failure direction to prefer --
     * silence is what this table exists to remove. */
    if (i == DCOMP_COMMIT_NOTE_SLOTS)
        i = next_slot++ % DCOMP_COMMIT_NOTE_SLOTS;
    notes[i].hwnd = hwnd;
    notes[i].note = note;
    return DCOMP_NOTE_NONE;
}

static void dcomp_commit_visual_tree(HWND target_hwnd, struct dcomp_visual *root)
{
    struct dcomp_leaf_stats stats = {0};
    BOOL rootless = FALSE;
    unsigned int idx = 0;
    ULONG_PTR gen;

    if (!root || !root->children || !target_hwnd)
    {
        enum dcomp_commit_note note;

        if (target_hwnd)
        {
            SetPropW(target_hwnd, L"__wine_dcomp_child_count", (HANDLE)0);
            KillTimer(target_hwnd, DCOMP_TREE_TIMER);
        }

        /* This return is the most frequent outcome in production and the only one
         * that said nothing at all.  Serum 2 and Trinity sit here permanently --
         * a content-bearing root with no children -- and telling that apart from
         * an empty tree had to be read off SetRoot/SetContent by hand during the
         * measurement series of 31.08.2026, because this path was silent.  The
         * distinction matters: "content-root without children" is a working tree
         * with nothing to composite under it, "root without content and without
         * children" is a tree that has not been built yet.
         *
         * The root predicate is deliberately the same two terms as the rootless
         * test further down, so the two reports cannot contradict each other. */
        if (!target_hwnd)
            note = DCOMP_NOTE_NO_TARGET;
        else if (!root)
            note = DCOMP_NOTE_NO_ROOT;
        else if (root->content || root->surface_content)
            note = DCOMP_NOTE_CONTENT_ROOT_ALONE;
        else
            note = DCOMP_NOTE_EMPTY_ROOT;

        if (dcomp_commit_note_set(target_hwnd, note) != note)
            FIXME("Commit skipped on target %p: %s -- nothing serialized, tree timer off.\n",
                    target_hwnd, dcomp_commit_note_text(note));
        return;
    }

    /* The leaf set is several properties written one by one, and wined3d reads
     * them from another thread: a reader can land between two of the writes and
     * take half of one commit and half of another.  The generation brackets the
     * set like a seqlock -- odd while this rewrite runs, advanced to the next
     * even value after the count -- so a reader can at least tell a torn set
     * from a settled one, and the absence of the property tells a new wined3d
     * that it is reading an old dcomp. */
    gen = (ULONG_PTR)GetPropW(target_hwnd, WINE_DCOMP_CHILD_GEN_PROP) | 1;
    SetPropW(target_hwnd, WINE_DCOMP_CHILD_GEN_PROP, (HANDLE)gen);

    /* The root's own content is presented by its swapchain/surface path and
     * must NOT be serialized as a child (it would composite onto itself) —
     * start the walk at its children. */
    {
        struct dcomp_visual *child;
        for (child = root->children; child; child = child->next_sibling)
            idx = dcomp_serialize_visual_leaves(target_hwnd, child,
                    (int)root->offset_x, (int)root->offset_y, idx, &stats);
    }

    SetPropW(target_hwnd, L"__wine_dcomp_child_count", (HANDLE)(ULONG_PTR)idx);
    SetPropW(target_hwnd, WINE_DCOMP_CHILD_GEN_PROP, (HANDLE)(gen + 1));

    /* Rootless tree (Chromium): no root Present will ever composite the
     * leaves. In-process targets get the 100ms timer as backstop; foreign-
     * process targets cannot be subclassed, so their only drive is this
     * hook (Chromium calls SetContent per frame). The hook-driven composite
     * also runs for in-process targets — rate-limited to ~60 Hz — so page
     * updates don't wait for the timer. With a content-bearing root the
     * swapchain Present path handles compositing. */
    /* The mode is a property of the root, not of the leaf count: a rootless tree
     * that serialized nothing this commit is still rootless, its timer is merely
     * idle.  The timer condition below therefore carries the extra leaf test. */
    rootless = !root->content && !root->surface_content;

    /* A target that changes mode changes which code composites its leaves, and
     * the throttled commit line below is silent from #6 to #100 -- a switch at
     * commit #20 would never show up there.  Reported on the change itself, so
     * the first commit stays the business of that line. */
    {
        enum dcomp_commit_note note = rootless ? DCOMP_NOTE_ROOTLESS : DCOMP_NOTE_CONTENT_ROOT;
        enum dcomp_commit_note prev = dcomp_commit_note_set(target_hwnd, note);

        if ((prev == DCOMP_NOTE_ROOTLESS || prev == DCOMP_NOTE_CONTENT_ROOT) && prev != note)
            FIXME("Mode switch on target %p: %s -> %s.\n", target_hwnd,
                    dcomp_commit_note_text(prev), dcomp_commit_note_text(note));
    }

    /* stats.total, not idx: what has to drive the timer is whether the tree has
     * content-bearing leaves at all, and idx counts only the serialized ones.
     * Since surface and texture leaves stopped being serialized, idx is zero for
     * exactly the trees this path exists for -- a rootless tree of texture
     * leaves would get neither the timer nor the composite below, and its
     * picture would never arrive.  Caught by dcomp-texture-test. */
    if (stats.total > 0 && rootless)
    {
        struct dcomp_target *target = (struct dcomp_target *)GetPropW(target_hwnd, dcomp_target_prop);
        DWORD now = GetTickCount();

        if (target && !target->foreign)
            SetTimer(target_hwnd, DCOMP_TREE_TIMER, dcomp_tree_timer_ms(), NULL);
        if (target && now - target->last_tree_composite_tick >= DCOMP_TREE_FRAME_MS)
        {
            target->last_tree_composite_tick = now;
            dcomp_target_composite_tree(target, FALSE);
        }
    }
    else
        KillTimer(target_hwnd, DCOMP_TREE_TIMER);

    /* The first question of every DComp diagnosis is which path this application
     * takes -- a rootless tree is driven by the timer and the SetContent hook,
     * a content-bearing root by the swapchain Present.  Instrumenting the wrong
     * one measures past the application (issue 178 cost four rounds that way).
     * The kinds matter for the same reason: only swapchain leaves survive the
     * wined3d reader, surface and texture leaves are dropped there. */
    if (stats.over_limit)
    {
        /* Own throttle: an overflow that first appears at commit 1234 has to be
         * reported then, not at the next multiple of 100. */
        static unsigned int overflow_count;

        if (++overflow_count <= 5 || !(overflow_count % 100))
            FIXME("Commit overflow #%u: target %p mode=%s serialized=%u total=%u limit=%u "
                    "dropped=%u surface=%u texture=%u swapchain=%u.\n",
                    overflow_count, target_hwnd, rootless ? "rootless" : "content-root",
                    idx, stats.total, DCOMP_MAX_SERIALIZED_LEAVES, stats.over_limit,
                    stats.surface, stats.texture, stats.swapchain);
    }
    else
    {
        static unsigned int commit_count;

        if (++commit_count <= 5 || !(commit_count % 100))
            FIXME("Commit #%u: target %p mode=%s serialized=%u total=%u "
                    "surface=%u texture=%u swapchain=%u.\n",
                    commit_count, target_hwnd, rootless ? "rootless" : "content-root",
                    idx, stats.total, stats.surface, stats.texture, stats.swapchain);
    }

    /* Under a content-bearing root the wined3d Present path is what composites
     * the tree, and it takes swapchain leaves only.  Surface and texture leaves
     * are therefore not drawn at all here -- pixels missing from the frame with
     * no error anywhere, and a search that starts in dcomp, where everything
     * looks right.  This line is what replaces the serialization removed on
     * 01.09.2026: the measurement of that day found no application in the field
     * that builds this combination, but the next one to build it says so here
     * instead of quietly losing its content.
     *
     * Rootless trees are not affected -- dcomp_target_composite_leaves() drives
     * them and knows all three kinds. */
    if (!rootless && !root->surface_content && (stats.surface || stats.texture))
    {
        static unsigned int uncomposited_count;

        if (++uncomposited_count <= 5 || !(uncomposited_count % 200))
            FIXME("Leaves not composited #%u: target %p carries %u surface and %u texture "
                    "leaves under a content-bearing root; the wined3d present path takes "
                    "swapchain leaves only, so their pixels never reach the frame.\n",
                    uncomposited_count, target_hwnd, stats.surface, stats.texture);
    }
    /* A surface root presents through dcomp_target_present_region(), which
     * composites its direct surface children itself (Cubase 15: 34 of them
     * under the project window's root surface, all delivered).  What that
     * path does not draw are texture leaves. */
    else if (!rootless && root->surface_content && stats.texture)
    {
        static unsigned int uncomposited_count;

        if (++uncomposited_count <= 5 || !(uncomposited_count % 200))
            FIXME("Leaves not composited #%u: target %p carries %u texture leaves under a "
                    "surface root; the GDI present composites surface leaves only, so "
                    "their pixels never reach the frame.\n",
                    uncomposited_count, target_hwnd, stats.texture);
    }
}

static void dcomp_device_remove_target(struct dcomp_device *device, struct dcomp_target *target)
{
    struct dcomp_target **pp;

    EnterCriticalSection(&device->cs);
    pp = &device->targets;
    while (*pp && *pp != target)
        pp = &(*pp)->next_target;
    if (*pp)
        *pp = target->next_target;
    target->next_target = NULL;
    target->device = NULL;
    LeaveCriticalSection(&device->cs);
}

static void dcomp_target_ensure_comp_dc(struct dcomp_target *target, UINT width, UINT height)
{
    BITMAPINFO bmi;

    /* Already have a matching DIB section */
    if (target->comp_dc && target->comp_width == width && target->comp_height == height)
        return;

    /* Clean up old */
    if (target->comp_bitmap)
    {
        SelectObject(target->comp_dc, NULL);
        DeleteObject(target->comp_bitmap);
        target->comp_bitmap = NULL;
        target->comp_bits = NULL;
    }
    if (target->comp_dc)
    {
        DeleteDC(target->comp_dc);
        target->comp_dc = NULL;
    }

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -(int)height;  /* top-down DIB */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    target->comp_dc = CreateCompatibleDC(NULL);
    if (!target->comp_dc)
        return;

    target->comp_bitmap = CreateDIBSection(target->comp_dc, &bmi,
            DIB_RGB_COLORS, (void **)&target->comp_bits, NULL, 0);
    if (!target->comp_bitmap)
    {
        DeleteDC(target->comp_dc);
        target->comp_dc = NULL;
        return;
    }

    SelectObject(target->comp_dc, target->comp_bitmap);
    target->comp_width = width;
    target->comp_height = height;
    target->comp_needs_full_present = TRUE;
    /* Fresh DIB section: zero-initialised, so it holds no usable backdrop yet
     * and must not be kept as one when the next capture fails (issue 116). */
    target->comp_backdrop_valid = FALSE;

    FIXME("Created comp DC %p with %ux%u DIB for target hwnd %p.\n",
            target->comp_dc, width, height, target->hwnd);
}

/* The save-under copy of the window content (issue 196), laid out like
 * ensure_comp_dc above.  A size change invalidates what was saved: the
 * backdrop belongs to another window geometry, so saved_rgn goes empty and
 * the next delivery re-captures from the window. */
static void dcomp_target_ensure_save_dc(struct dcomp_target *target, UINT width, UINT height)
{
    BITMAPINFO bmi;

    if (target->save_dc && target->save_width == width && target->save_height == height)
        return;

    if (target->save_bitmap)
    {
        SelectObject(target->save_dc, NULL);
        DeleteObject(target->save_bitmap);
        target->save_bitmap = NULL;
        target->save_bits = NULL;
    }
    if (target->save_dc)
    {
        DeleteDC(target->save_dc);
        target->save_dc = NULL;
    }
    if (target->saved_rgn)
        SetRectRgn(target->saved_rgn, 0, 0, 0, 0);

    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -(int)height;  /* top-down DIB */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    target->save_dc = CreateCompatibleDC(NULL);
    if (!target->save_dc)
        return;

    target->save_bitmap = CreateDIBSection(target->save_dc, &bmi,
            DIB_RGB_COLORS, (void **)&target->save_bits, NULL, 0);
    if (!target->save_bitmap)
    {
        DeleteDC(target->save_dc);
        target->save_dc = NULL;
        return;
    }

    SelectObject(target->save_dc, target->save_bitmap);
    target->save_width = width;
    target->save_height = height;
}

/* -------------------------------------------------------------------------
 * Target-side tree compositing for rootless visual trees (Chromium/WebView2,
 * issue 88). When the root visual carries no content of its own, no root
 * Present ever runs, so the serialized leaf visuals would stay invisible.
 * dcomp_target_composite_tree() walks the tree like the serializer, fetches
 * each leaf's comp bits (DComp surface: bits directly; swapchain content:
 * the __wine_dcomp_comp_bits/__wine_dcomp_comp_size props wined3d maintains
 * on the leaf's hidden comp window in child mode) and Porter-Duff-Over-
 * composites them into the target DIB, then blits to the target window.
 * Driven by DCOMP_TREE_TIMER (armed by dcomp_commit_visual_tree) and by
 * WM_PAINT on the target hwnd.
 */

/* Unchanged-content gate (issue 99): skip TIMER-driven blits when no
 * content-leaf source changed since the last frame that reached the window.
 * The host repaints the panel exactly once on a tab switch while the hide is
 * still in flight through Chromium's IPC — a re-blit of the unchanged (fully
 * opaque) composition wins that race and wallpapers over the host's
 * painting.  Exposure repaints stay covered by the WM_PAINT path, which is
 * never skipped.  Default ON, WINE_DCOMP_SKIP_UNCHANGED=0 opts out. */
static int dcomp_skip_unchanged_enabled = -1;

static BOOL dcomp_skip_unchanged(void)
{
    if (dcomp_skip_unchanged_enabled < 0)
    {
        const char *e = getenv("WINE_DCOMP_SKIP_UNCHANGED");
        dcomp_skip_unchanged_enabled = (!e || atoi(e)) ? 1 : 0;
    }
    return dcomp_skip_unchanged_enabled > 0;
}

/* Sparse FNV-1a over a composition buffer, RGB only (alpha carries noise).
 * Used to tell whether the window still shows the frame we delivered, so it
 * must sample the same way on both sides of the comparison. */
static DWORD dcomp_surface_hash(const DWORD *bits, unsigned int count)
{
    DWORD h = 2166136261u;
    unsigned int i;

    for (i = 0; i < count; i += 64)
        h = (h ^ (bits[i] & 0x00ffffff)) * 16777619u;
    return h;
}

/* Host-restore gate (issue 99): when our target window hides or changes
 * geometry, hand the vacated screen area back to the host for repaint —
 * child targets share an ancestor's X11 drawable, so pixels we blitted stay
 * physically behind and nobody else repaints them (the host clips around
 * what it believes is a self-drawing child; on Windows a hidden child simply
 * stops being composed).  Default ON, WINE_DCOMP_HOST_RESTORE=0 opts out. */
static int dcomp_host_restore_enabled = -1;

static BOOL dcomp_host_restore(void)
{
    if (dcomp_host_restore_enabled < 0)
    {
        const char *e = getenv("WINE_DCOMP_HOST_RESTORE");
        dcomp_host_restore_enabled = (!e || atoi(e)) ? 1 : 0;
    }
    return dcomp_host_restore_enabled > 0;
}

static BOOL dcomp_wnd_is_chromium(HWND wnd)
{
    WCHAR cls[64];

    if (!GetClassNameW(wnd, cls, ARRAY_SIZE(cls)))
        return FALSE;
    return !wcsncmp(cls, L"Chrome_", 7)
            || !wcscmp(cls, L"Intermediate D3D Window");
}

/* Invalidate a screen-space area on the first visible non-Chromium ancestor
 * of the target (Chromium windows never GDI-paint, so an invalidation there
 * would be swallowed).  After a resize the vacated area may no longer belong
 * to our target's branch of the window tree at all (the panel shrank — the
 * strip now lies over a SIBLING branch of the host), so repeat the
 * invalidation from the top-level: RDW_ALLCHILDREN covers every branch.
 * Asynchronous on purpose (no RDW_UPDATENOW) — this runs in the content
 * process and the ancestors belong to the host. */
static void dcomp_restore_area_to_host(struct dcomp_target *target,
        const RECT *screen_rc, const char *why)
{
    HWND owner = GetAncestor(target->hwnd, GA_PARENT);
    HWND desktop = GetDesktopWindow();
    HWND top;
    RECT r = *screen_rc;

    while (owner && owner != desktop
            && (!IsWindowVisible(owner) || dcomp_wnd_is_chromium(owner)))
        owner = GetAncestor(owner, GA_PARENT);
    if (!owner || owner == desktop)
        return;
    MapWindowPoints(NULL, owner, (POINT *)&r, 2);
    RedrawWindow(owner, &r, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);

    top = GetAncestor(target->hwnd, GA_ROOT);
    if (top && top != owner)
    {
        r = *screen_rc;
        MapWindowPoints(NULL, top, (POINT *)&r, 2);
        RedrawWindow(top, &r, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    }

    TRACE("issue-99 restore-%s: gave (%ld,%ld)-(%ld,%ld) back to host ancestor %p top %p "
            "(target %p hwnd %p).\n", why,
            (long)screen_rc->left, (long)screen_rc->top,
            (long)screen_rc->right, (long)screen_rc->bottom,
            owner, top, target, target->hwnd);
}

static void dcomp_composite_premul_over(DWORD *dst_bits, UINT dst_w, UINT dst_h,
        const DWORD *src_bits, UINT src_w, UINT src_h, int ox, int oy)
{
    int src_x = (ox < 0) ? -ox : 0;
    int src_y = (oy < 0) ? -oy : 0;
    int dst_x = (ox < 0) ? 0 : ox;
    int dst_y = (oy < 0) ? 0 : oy;
    int copy_w = min((int)src_w - src_x, (int)dst_w - dst_x);
    int copy_h = min((int)src_h - src_y, (int)dst_h - dst_y);
    int x, y;

    if (copy_w <= 0 || copy_h <= 0)
        return;

    for (y = 0; y < copy_h; y++)
    {
        DWORD *dst_row = dst_bits + (dst_y + y) * dst_w + dst_x;
        const DWORD *src_row = src_bits + (src_y + y) * src_w + src_x;
        for (x = 0; x < copy_w; x++)
        {
            DWORD s = src_row[x];
            BYTE sa = (s >> 24);
            if (sa == 0xff)
            {
                dst_row[x] = s;
            }
            else if (sa > 0)
            {
                /* Premultiplied alpha: out = src + dst * (1 - sa/255) */
                DWORD d = dst_row[x];
                BYTE ia = 255 - sa;
                dst_row[x] = ((min(sa + (((d >> 24) * ia + 127) / 255), 255u)) << 24)
                           | ((((s >> 16) & 0xff) + ((((d >> 16) & 0xff) * ia + 127) / 255)) << 16)
                           | ((((s >> 8) & 0xff) + ((((d >> 8) & 0xff) * ia + 127) / 255)) << 8)
                           | (((s & 0xff) + (((d & 0xff) * ia + 127) / 255)));
            }
        }
    }
}

/* Bring the bits the composition reads up to date with what the application
 * drew.  EndDraw only records the dirty region and leaves the GPU→CPU readback
 * to the present (issue 56), but that present only ever runs for a surface on
 * the *root* visual: both call sites of dcomp_target_flush_present() skip a
 * target whose root carries no content.  A surface hanging on a child visual
 * was therefore composited out of a buffer nothing ever filled -- measured on
 * Studio Pro 8, whose entire tree is three such leaves: 1590 walks of them, not
 * one non-transparent pixel, while the application was drawing all along.
 * This is the surface counterpart of dcomp_texture_ensure_bits() below. */
static void dcomp_surface_ensure_bits(struct dcomp_surface *surface)
{
    LONG l, t, r, b;

    if (!surface->has_pending || !surface->bits || !surface->width || !surface->height)
        return;

    l = surface->pending_dirty.left;   t = surface->pending_dirty.top;
    r = surface->pending_dirty.right;  b = surface->pending_dirty.bottom;
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > (LONG)surface->width)  r = (LONG)surface->width;
    if (b > (LONG)surface->height) b = (LONG)surface->height;

    dcomp_surface_readback_region(surface, l, t, r, b);
    surface->has_pending = FALSE;
    SetRectEmpty(&surface->pending_dirty);
}

static void dcomp_target_composite_leaves(struct dcomp_target *target, struct dcomp_visual *visual,
        int base_x, int base_y)
{
    struct dcomp_visual *child;
    int vx = base_x + (int)visual->offset_x;
    int vy = base_y + (int)visual->offset_y;

    if (visual->surface_content && visual->surface_content->bits)
    {
        /* Not hashed — never skip-unchanged this tree. */
        target->walk_leaf_hash_valid = FALSE;
        dcomp_surface_ensure_bits(visual->surface_content);
        dcomp_composite_premul_over(target->comp_bits, target->comp_width, target->comp_height,
                visual->surface_content->bits, visual->surface_content->width,
                visual->surface_content->height, vx, vy);
    }
    else if (dcomp_visual_effective_texture(visual))
    {
        /* Composition texture leaf (issue 90) or dynamic texture frame (issue 95) */
        struct dcomp_texture *tex = dcomp_visual_effective_texture(visual);

        /* Not hashed — never skip-unchanged this tree. */
        target->walk_leaf_hash_valid = FALSE;

        dcomp_texture_ensure_bits(tex);
        if (tex->bits)
        {
            dcomp_composite_premul_over(target->comp_bits, target->comp_width, target->comp_height,
                    tex->bits, tex->bits_width, tex->bits_height, vx, vy);
        }
    }
    else if (visual->content)
    {
        WCHAR prop_name[64];
        HWND comp_wnd;
        DWORD *bits;
        ULONG_PTR dims;

        /* Same lookup the serializer uses: comp window via desktop prop. */
        swprintf(prop_name, ARRAY_SIZE(prop_name),
                WINE_DCOMP_WND_PROP_FMT, GetCurrentProcessId(), (UINT_PTR)visual->content);
        comp_wnd = (HWND)GetPropW(GetDesktopWindow(), prop_name);
        if (comp_wnd)
        {
            bits = (DWORD *)GetPropW(comp_wnd, L"__wine_dcomp_comp_bits");
            dims = (ULONG_PTR)GetPropW(comp_wnd, L"__wine_dcomp_comp_size");
            if (bits && dims)
            {
                /* Unchanged-content gate: fold this leaf's source pixels and
                 * placement into the walk hash.  RGB only (alpha carries
                 * noise); every 4th pixel is plenty to tell a new frame from
                 * a re-presented one. */
                if (target->walk_leaf_hash_valid && dcomp_skip_unchanged())
                {
                    UINT64 lt = (UINT64)LOWORD(dims) * HIWORD(dims);
                    DWORD h = target->walk_leaf_hash;
                    unsigned int i;

                    for (i = 0; i < (unsigned int)lt; i += 4)
                        h = (h ^ (bits[i] & 0x00ffffff)) * 16777619u;
                    h = (h ^ (DWORD)(ULONG_PTR)comp_wnd) * 16777619u;
                    h = (h ^ (DWORD)dims) * 16777619u;
                    h = (h ^ (DWORD)(vx * 65599 + vy)) * 16777619u;
                    target->walk_leaf_hash = h;
                }
                dcomp_composite_premul_over(target->comp_bits, target->comp_width,
                        target->comp_height, bits, LOWORD(dims), HIWORD(dims), vx, vy);
            }
        }
    }

    for (child = visual->children; child; child = child->next_sibling)
        dcomp_target_composite_leaves(target, child, vx, vy);
}

/* Does any visual below this one carry content of its own?  Pointer walk over
 * the same three leaf kinds dcomp_target_composite_leaves() knows, but without
 * their cost: no property lookups, no texture readback, no pixels.  A leaf whose
 * source is not resolvable yet still counts as content — it will be, and
 * answering "no content" too eagerly is the expensive mistake (issue 184). */
static BOOL dcomp_visual_subtree_has_content(struct dcomp_visual *visual)
{
    struct dcomp_visual *child;

    if (visual->content || visual->surface_content || dcomp_visual_effective_texture(visual))
        return TRUE;

    for (child = visual->children; child; child = child->next_sibling)
        if (dcomp_visual_subtree_has_content(child))
            return TRUE;

    return FALSE;
}

/* Is this target entitled to paint the window — that is, does our composition
 * have anything to put there (issue 184)?
 *
 * A rootless target subclasses the window, swallows its WM_PAINT and blits our
 * composed tree in its place.  That is right where our composition IS the
 * content (WebView2, issue 88).  With a tree that never held any content there
 * is nothing to put in the message's place: the application is never asked to
 * redraw, stops presenting, and its last frame stands still on screen —
 * Fender Studio Pro 8, whose target sits on the very window it presents to
 * itself and which never calls SetContent at all (issue 183).
 *
 * "Empty right now" is the wrong question: a WebView2 tree drops to zero leaves
 * between two generations (measured: commit #1 one leaf, commit #2 none), and
 * releasing the window in those frames would let the application paint over our
 * composition.  So the verdict latches on first content and never goes back —
 * from then on this target behaves exactly as it did before.  Only a tree that
 * has never carried a single leaf stays out of the way. */
static BOOL dcomp_target_tree_carries_content(struct dcomp_target *target)
{
    struct dcomp_visual *child;

    if (target->tree_had_content)
        return TRUE;

    /* The root's own content is presented by its swapchain/surface path and is
     * not ours to composite — start at its children, like the walks do. */
    if (target->root_visual)
    {
        for (child = target->root_visual->children; child; child = child->next_sibling)
        {
            if (dcomp_visual_subtree_has_content(child))
            {
                target->tree_had_content = TRUE;
                return TRUE;
            }
        }
    }

    return FALSE;
}

/* Pixel extent this leaf would occupy in the composition, derived the same way
 * dcomp_target_composite_leaves() derives it — but without paying for it: no
 * texture readback, no pixel pointer, only the dimensions.  FALSE when the
 * visual carries no content of its own or its size is not known yet. */
static BOOL dcomp_visual_leaf_extent(struct dcomp_visual *visual, UINT *w, UINT *h)
{
    struct dcomp_texture *tex;

    if (visual->surface_content)
    {
        *w = visual->surface_content->width;
        *h = visual->surface_content->height;
        return *w && *h;
    }
    if ((tex = dcomp_visual_effective_texture(visual)))
    {
        UINT x, y;

        *w = tex->desc.Width;
        *h = tex->desc.Height;
        if (tex->has_source_rect)
        {
            x = min(tex->source_rect.left, tex->desc.Width);
            y = min(tex->source_rect.top, tex->desc.Height);
            *w = min(tex->source_rect.right, tex->desc.Width) - x;
            *h = min(tex->source_rect.bottom, tex->desc.Height) - y;
        }
        return (int)*w > 0 && (int)*h > 0;
    }
    if (visual->content)
    {
        WCHAR prop_name[64];
        HWND comp_wnd;
        ULONG_PTR dims;

        swprintf(prop_name, ARRAY_SIZE(prop_name),
                WINE_DCOMP_WND_PROP_FMT, GetCurrentProcessId(), (UINT_PTR)visual->content);
        if (!(comp_wnd = (HWND)GetPropW(GetDesktopWindow(), prop_name)))
            return FALSE;
        if (!(dims = (ULONG_PTR)GetPropW(comp_wnd, L"__wine_dcomp_comp_size")))
            return FALSE;
        *w = LOWORD(dims);
        *h = HIWORD(dims);
        return *w && *h;
    }
    return FALSE;
}

/* -------------------------------------------------------------------------
 * When the takeover happens, and how far anything below it reaches
 * (issue 187, issue 205).
 *
 * dcomp_target_tree_carries_content() answers WHETHER this target may paint
 * the window.  That verdict is binary, and the consequence used to be total:
 * the whole window blitted from our composition, its whole WM_PAINT swallowed.
 * For a tree that IS the window (WebView2, issue 88) that is right.  Fender
 * Studio Pro 8 shows what it costs otherwise -- three one-pixel strips in a
 * 1920x1027 window, 0.03% coverage, and the application is never asked to
 * repaint the remaining 99.97% again.  It scrolls its arranger, the exposed
 * area is never redrawn, and the old pixels pile up into a stripe pattern.
 *
 * DCOMP_MIN_COVER_PERCENT answers WHEN that verdict may flip: the leaf region
 * of the CURRENT frame has to cover half the client area first (issue 205).
 * The lifetime union covered_rgn used to be asked instead, and a moving leaf
 * grew it across the threshold one position at a time -- the region is still
 * maintained, but only for the WINE_DCOMP_COVER_FRAME=0 counter-check.  A
 * tree that legitimately covers its window does so in every frame, so
 * measuring per frame keeps the two kinds of tree three orders of magnitude
 * apart, permanently rather than until enough positions accumulated.
 *
 * HOW FAR anything reaches is per-area only below the threshold: the delivery
 * (issue 190) answers it with this frame's region, exactly the area we put
 * pixels into.  The takeover itself is all or nothing.
 *
 * Latched is the verdict alone: once covers_window is TRUE it stays TRUE, so
 * a rootless tree momentarily empty between two generations cannot revoke a
 * takeover mid-flight (issue 184).  Extending that latch to the measurement
 * was the defect issue 205 closed.
 *
 * A leaf whose extent is not resolvable yet (a swapchain leaf before wined3d
 * published its comp size) claims the whole client area instead of nothing --
 * being too eager to disown an area is the expensive mistake, and the fallback
 * is exactly the behaviour this code had before.  The threshold region
 * contains that fallback; frame_rgn, which the delivery reads, deliberately
 * does not.
 */
static UINT64 dcomp_region_area(HRGN region)
{
    UINT64 area = 0;
    RGNDATA *data;
    DWORD size;
    DWORD i;

    if (!(size = GetRegionData(region, 0, NULL)))
        return 0;
    if (!(data = malloc(size)))
        return 0;
    if (GetRegionData(region, size, data))
    {
        const RECT *r = (const RECT *)data->Buffer;

        for (i = 0; i < data->rdh.nCount; i++)
            area += (UINT64)(r[i].right - r[i].left) * (r[i].bottom - r[i].top);
    }
    free(data);
    return area;
}

/* Share of the client area the tree has to cover before this target takes the
 * window over at all.  The two kinds of tree are three orders of magnitude
 * apart, so the exact number matters little: WebView2 and the trees like it
 * put the page itself into one leaf and cover essentially the whole window,
 * while Fender Studio Pro 8 covers 0.03%.  Anything between them separates the
 * cases; half the window says plainly what the takeover assumes, namely that
 * our composition IS what the window shows.  WINE_DCOMP_MIN_COVER overrides it
 * (percent, 0 disables the check). */
#define DCOMP_MIN_COVER_PERCENT 50

static int dcomp_min_cover_percent = -1;

static int dcomp_min_cover(void)
{
    if (dcomp_min_cover_percent < 0)
    {
        const char *e = getenv("WINE_DCOMP_MIN_COVER");
        dcomp_min_cover_percent = e ? atoi(e) : DCOMP_MIN_COVER_PERCENT;
        if (dcomp_min_cover_percent < 0)
            dcomp_min_cover_percent = 0;
    }
    return dcomp_min_cover_percent;
}

/* Which region the takeover threshold below is asked of (issue 205).  The
 * verdict latches either way -- only the measurement differs.  Default on:
 * this frame's leaf region, so a moving leaf cannot grow its way across the
 * threshold one position at a time.  WINE_DCOMP_COVER_FRAME=0 restores the
 * lifetime-union measurement of 6220c00ff4b for counter-checks. */
static int dcomp_cover_frame_enabled = -1;

static BOOL dcomp_cover_frame(void)
{
    if (dcomp_cover_frame_enabled < 0)
    {
        const char *e = getenv("WINE_DCOMP_COVER_FRAME");
        dcomp_cover_frame_enabled = (!e || atoi(e)) ? 1 : 0;
    }
    return dcomp_cover_frame_enabled > 0;
}

static void dcomp_target_collect_covered(struct dcomp_visual *visual, int base_x, int base_y,
        HRGN region, BOOL *unresolved)
{
    struct dcomp_visual *child;
    int vx = base_x + (int)visual->offset_x;
    int vy = base_y + (int)visual->offset_y;
    UINT w, h;

    if (dcomp_visual_leaf_extent(visual, &w, &h))
    {
        HRGN leaf = CreateRectRgn(vx, vy, vx + (int)w, vy + (int)h);

        if (leaf)
        {
            CombineRgn(region, region, leaf, RGN_OR);
            DeleteObject(leaf);
        }
        else *unresolved = TRUE;
    }
    else if (visual->content || visual->surface_content || dcomp_visual_effective_texture(visual))
    {
        /* Carries content by the same test the latch uses, but will not say how
         * large it is yet. */
        *unresolved = TRUE;
    }

    for (child = visual->children; child; child = child->next_sibling)
        dcomp_target_collect_covered(child, vx, vy, region, unresolved);
}

static void dcomp_target_update_covered(struct dcomp_target *target, const RECT *client_rc)
{
    struct dcomp_visual *child;
    BOOL unresolved = FALSE;
    HRGN current;

    if (!target->root_visual)
        return;

    if (!(current = CreateRectRgn(0, 0, 0, 0)))
        return;

    for (child = target->root_visual->children; child; child = child->next_sibling)
        dcomp_target_collect_covered(child, (int)target->root_visual->offset_x,
                (int)target->root_visual->offset_y, current, &unresolved);

    /* Only the part inside the client area counts (issue 298, review C5).  A
     * leaf larger than the window, at a negative offset or reaching past the
     * right or bottom edge used to contribute area nobody can see, and the
     * takeover threshold compared that area against the client size: 50%
     * could be crossed with next to nothing visibly covered, and
     * covers_window never goes back.  The delivery cannot paint outside the
     * window either, so every consumer of this region wants the clipped one.
     * The unresolved fallback below is built from client_rc and needs none. */
    {
        HRGN client_rgn = CreateRectRgn(0, 0, client_rc->right, client_rc->bottom);

        if (client_rgn)
        {
            CombineRgn(current, current, client_rgn, RGN_AND);
            DeleteObject(client_rgn);
        }
    }

    /* Keep this frame's own leaf region (issue 190), taken before the
     * unresolved fallback below widens it to the whole client area: it is
     * exactly the area we can put pixels into, and the delivery path needs it
     * unlatched -- covered_rgn grows with every position a moving leaf has ever
     * had, and delivering that would put us back to reading and rewriting an
     * area we do not draw. */
    if (!target->frame_rgn)
        target->frame_rgn = CreateRectRgn(0, 0, 0, 0);
    if (target->frame_rgn)
        CombineRgn(target->frame_rgn, current, NULL, RGN_COPY);

    if (unresolved)
    {
        HRGN all = CreateRectRgn(0, 0, client_rc->right, client_rc->bottom);

        if (all)
        {
            CombineRgn(current, current, all, RGN_OR);
            DeleteObject(all);
        }
    }

    /* The lifetime union, still maintained for the WINE_DCOMP_COVER_FRAME=0
     * measurement below (issue 205). */
    if (!target->covered_rgn)
        target->covered_rgn = CreateRectRgn(0, 0, 0, 0);
    if (target->covered_rgn)
        CombineRgn(target->covered_rgn, target->covered_rgn, current, RGN_OR);

    /* The verdict latches, the measurement does not (issue 205).  Asking the
     * threshold of covered_rgn made the union a latch with a timer: a leaf
     * 1 px wide and 821 px tall covers 0.03% of every frame in a 1920x1027
     * window, but after ~1200 distinct columns -- about 25 s of transport at
     * 100 px per bar -- the accumulated positions alone cross the 50% mark,
     * and a tree that never covered more than a sliver of the window takes
     * it over for good.  This frame's region cannot grow that way: a tree
     * that legitimately covers its window does so in every frame (that is
     * what the 100% measurements behind DCOMP_MIN_COVER_PERCENT say), so the
     * three orders of magnitude between the two kinds of tree hold per frame
     * instead of eroding over time.
     *
     * `current`, not frame_rgn: the threshold region must contain the
     * unresolved fallback above -- a swapchain leaf before its first present
     * has no extent, and the fallback claiming the client area for it is
     * what lets such trees take over at all.  frame_rgn deliberately does
     * not contain it, because the delivery path must not read and rewrite
     * an area it does not draw.  Two questions, two regions.
     *
     * What stays latched is the verdict itself: covers_window never goes
     * back, so a rootless tree running empty between two generations cannot
     * revoke a takeover mid-flight (issue 184). */
    if (!target->covers_window)
    {
        UINT64 client = (UINT64)client_rc->right * client_rc->bottom;
        UINT64 cover;

        if (!dcomp_min_cover())
            target->covers_window = TRUE;
        else if (client && (cover = dcomp_region_area(dcomp_cover_frame()
                ? current : target->covered_rgn)) * 100
                >= client * dcomp_min_cover())
        {
            target->covers_window = TRUE;
            FIXME("Target %p hwnd %p: tree covers %I64u of %I64u client pixels, taking the "
                    "window over.\n", target, target->hwnd, cover, client);
        }
    }

    DeleteObject(current);
}

/* Blit a region rectangle by rectangle.  Clipping the destination and running
 * one full-window BitBlt paints the same pixels, but still makes the driver
 * fetch the whole source -- for the read-back that is an X GetImage over the
 * entire window, which is precisely the cost this path exists to avoid. */
static BOOL dcomp_blt_region(HDC dst, HDC src, HRGN region)
{
    BOOL ok = FALSE;
    RGNDATA *data;
    DWORD size;
    DWORD i;

    if (!(size = GetRegionData(region, 0, NULL)))
        return FALSE;
    if (!(data = malloc(size)))
        return FALSE;
    if (GetRegionData(region, size, data))
    {
        const RECT *r = (const RECT *)data->Buffer;

        ok = TRUE;
        for (i = 0; i < data->rdh.nCount; i++)
        {
            if (!BitBlt(dst, r[i].left, r[i].top, r[i].right - r[i].left,
                    r[i].bottom - r[i].top, src, r[i].left, r[i].top, SRCCOPY))
                ok = FALSE;
        }
    }
    free(data);
    return ok;
}

/* -------------------------------------------------------------------------
 * Delivering the covered region without claiming the window (issue 190).
 *
 * The threshold above answers whether our composition IS what the window
 * shows.  For Fender Studio Pro 8 it plainly is not -- three one-pixel leaves
 * in a 1920x1027 window -- and claiming the window on their account is what
 * issue 187 had to undo.  But those three strips are the transport playhead,
 * and nobody else draws them: below the threshold we delivered nothing at all,
 * so the line was simply missing while the transport ran.
 *
 * Both are avoidable, because the two consequences of the takeover were never
 * one thing.  Delivering pixels is harmless; taking WM_PAINT away is what goes
 * stale.  So this reads back, blends and blits ONLY the rectangles the tree
 * covers -- three one-pixel columns instead of two million pixels -- and leaves
 * the message with the application.  Nothing is taken from it, so nothing goes
 * unrepainted, and neither of us writes pixels the other owns.
 *
 * This frame's region, never covered_rgn: that one latches (issue 184), and a
 * playhead walks across the window, so within seconds it would be a wide band
 * -- reading and rewriting an area we do not draw, which is the takeover again
 * under another name.
 *
 * What we stop covering we hand back.  A leaf that moved or vanished leaves its
 * pixels standing otherwise, and at sixty frames a second that is the stripe
 * pattern of issue 187 with the roles reversed.  Invalidating the difference
 * lets the application repaint it -- which it can, because its WM_PAINT is
 * still its own.  The playhead disappearing when the transport stops is that
 * same mechanism and needs no case of its own.
 *
 * WINE_DCOMP_REGION_DELIVER=0 falls back to delivering nothing below the
 * threshold, for counter-checks. */
static int dcomp_region_delivery_enabled = -1;

/* How the delivery blit is pushed out to the display server (issue 206).
 *
 * MEASURED on Fender Studio Pro 8, same song, transport running, share of
 * captured frames missing the playhead column (20 s per run at 20 fps):
 *
 *   0  off                      79.0 %
 *   3  GdiFlush() only          not sufficient -- drains Wine's GDI batch, after
 *                               which the requests still sit in Xlib's buffer
 *   1  XFlush via escape         1.5 / 2.8 %      (median 2.2)   <- default
 *   4  XSync via escape          2.5 / 4.2 / 4.0  (median 4.0)
 *   2  full round trip           0.5 / 0.8 / 7.2  (median 0.8)
 *
 * The round trip wins, which is not obvious: XSync also waits for the server but
 * blocks on the whole queue, while reading one pixel only waits for this
 * drawable.  The ordering between 1, 2 and 4 is within the run-to-run spread and
 * has not been settled; 2 was chosen because it is the one where the flicker was
 * visually all but gone.
 *
 * A second machine reversed that (issue 206, laptop: i7-5600U, Intel HD 5500 /
 * Mesa, X11).  Timing the deliveries there showed a FLOOR that has nothing to do
 * with how much is delivered: 442 px already cost 4.7-6 ms in mode 2 against
 * 0.6 ms in mode 1, and area explains little on top of that -- a factor of 754
 * in area buys a factor of 2.3 in duration.  That floor is the round trip
 * itself, and its size is a property of the machine, not of this code.  Where it
 * is expensive, waiting for it costs more than it saves: with the transport
 * stopped mode 1 is visibly calmer there, while during a drag the two are
 * indistinguishable.
 *
 * Hence 1 is the default now.  The desktop numbers above were never reproduced
 * with the floor known, and they rest on a difference inside their own spread --
 * issue 216 measures the floor there, and if it turns out small, mode 2 is the
 * better default on that hardware and this needs to become adaptive rather than
 * a single constant. */
static int dcomp_deliver_flush_mode = -1;

static int dcomp_deliver_flush(void)
{
    if (dcomp_deliver_flush_mode < 0)
    {
        const char *e = getenv("WINE_DCOMP_DELIVER_FLUSH");
        dcomp_deliver_flush_mode = e ? atoi(e) : 1;
        if (dcomp_deliver_flush_mode < 0)
            dcomp_deliver_flush_mode = 0;
    }
    return dcomp_deliver_flush_mode;
}

static BOOL dcomp_region_delivery(void)
{
    if (dcomp_region_delivery_enabled < 0)
    {
        const char *e = getenv("WINE_DCOMP_REGION_DELIVER");
        dcomp_region_delivery_enabled = (!e || atoi(e)) ? 1 : 0;
    }
    return dcomp_region_delivery_enabled > 0;
}

/* Restoring vacated pixels ourselves instead of only invalidating them
 * (issue 196).  Default on; WINE_DCOMP_SAVE_UNDER=0 turns the cleanup back
 * into the delegated InvalidateRgn of 991b3cc22a6 for counter-checks. */
static int dcomp_save_under_enabled = -1;

static BOOL dcomp_save_under(void)
{
    if (dcomp_save_under_enabled < 0)
    {
        const char *e = getenv("WINE_DCOMP_SAVE_UNDER");
        dcomp_save_under_enabled = (!e || atoi(e)) ? 1 : 0;
    }
    return dcomp_save_under_enabled > 0;
}

/* --- The leaves in the frame instead of on the window (issue 206) ----------
 *
 * There are two delivery routes for DirectComposition content, and only one of
 * them is a race.  Above DCOMP_MIN_COVER_PERCENT the tree is composited into
 * the present buffer and is part of the frame; below it, it used to go to the
 * window as a blit of its own, after the present.  A transport playhead is 442
 * of some two million pixels, so it always took the second route -- and there
 * every swap landing between two deliveries wrote it away again.  Measured on
 * Fender Studio Pro 8: the line was missing from 48.2% of the captured frames.
 *
 * Now the leaves are published as a layer with alpha and wined3d draws them
 * into the frame it is about to present, which is the route DirectComposition
 * content above the threshold takes anyway and the one the DWM takes for all
 * of it.  Same measurement, same binary, same session: 0.2%.
 *
 * The layer is published only while a swapchain says it is going to draw it
 * (the sink property, maintained by wined3d).  Without one -- no wined3d
 * swapchain on this window at all, or one presenting through Vulkan, which has
 * no composite step -- the layer would be published and never drawn, and the
 * leaves would be missing not from half the frames but from all of them.  The
 * blit route stays for exactly that case.
 *
 * WINE_DCOMP_FRAME_COMPOSITE=0 forces the blit route back on.
 */
static int dcomp_frame_composite_mode = -1;

static BOOL dcomp_frame_composite(void)
{
    if (dcomp_frame_composite_mode < 0)
    {
        const char *e = getenv("WINE_DCOMP_FRAME_COMPOSITE");
        dcomp_frame_composite_mode = (!e || atoi(e)) ? 1 : 0;
    }
    return dcomp_frame_composite_mode > 0;
}

static void dcomp_target_retract_layer(struct dcomp_target *target);

/* How many deliveries a published layer may go undrawn before we stop trusting
 * the sink, and how many deliveries later we try it again.  The first has to
 * survive a stall -- a song load or a garbage collection can hold up presents
 * for a good second, and a transient must not switch the fix off for good.  The
 * second is what keeps such a fallback from being permanent; it costs one
 * delivery in six hundred, and only in a configuration that is already broken. */
#define DCOMP_LAYER_STALL 200
#define DCOMP_LAYER_RETRY 600

/* Is anybody going to draw a layer we publish -- and are they doing it?
 *
 * The sink property says a swapchain on this window can composite, which is not
 * the same as it presenting.  Three of Studio Pro 8's seven swapchains never
 * present a single frame, and a target on such a window would publish into the
 * void: not a flicker but a leaf gone entirely.  So the sink only arms the
 * route, and what keeps it is the compositor's draw count moving. */
static BOOL dcomp_target_use_layer_route(struct dcomp_target *target)
{
    ULONG_PTR sink;
    LONG drawn;

    if (!dcomp_frame_composite() || !target->hwnd)
        return FALSE;

    if (!(sink = (ULONG_PTR)GetPropW(target->hwnd, WINE_DCOMP_LAYER_SINK_PROP)))
    {
        /* No compositor on this window at all -- a Vulkan swapchain, or none. */
        dcomp_target_retract_layer(target);
        target->layer_route_off = FALSE;
        target->layer_unseen = target->layer_retry = 0;
        return FALSE;
    }

    if (target->layer_route_off)
    {
        if (sink != target->layer_sink_seen || ++target->layer_retry >= DCOMP_LAYER_RETRY)
        {
            TRACE("hwnd %p: trying the layer route again.\n", target->hwnd);
            target->layer_route_off = FALSE;
            target->layer_unseen = target->layer_retry = 0;
            if (target->layer)
                target->layer_drawn_seen = InterlockedCompareExchange(&target->layer->drawn, 0, 0);
            return TRUE;
        }
        return FALSE;
    }

    /* Only judge once something is actually published -- before that there is
     * nothing for anyone to draw. */
    if (target->layer && target->layer_live)
    {
        drawn = InterlockedCompareExchange(&target->layer->drawn, 0, 0);
        if (drawn != target->layer_drawn_seen)
        {
            target->layer_drawn_seen = drawn;
            target->layer_unseen = 0;
        }
        else if (++target->layer_unseen >= DCOMP_LAYER_STALL)
        {
            WARN("hwnd %p: the layer went undrawn through %u deliveries, blitting again.\n",
                    target->hwnd, target->layer_unseen);
            dcomp_target_retract_layer(target);
            target->layer_route_off = TRUE;
            target->layer_sink_seen = sink;
            target->layer_unseen = target->layer_retry = 0;
            return FALSE;
        }
    }
    return TRUE;
}

/* The region as up to WINE_DCOMP_LAYER_MAX_RECTS rectangles, or its bounding
 * box if it has more.  Returns the number written, 0 for an empty region. */
static unsigned int dcomp_layer_region_rects(HRGN rgn, RECT *out, const RECT *clip)
{
    unsigned int n = 0, i;
    RGNDATA *data;
    DWORD size;
    RECT box;

    if (GetRgnBox(rgn, &box) <= NULLREGION)
        return 0;

    if ((size = GetRegionData(rgn, 0, NULL)) && (data = malloc(size)))
    {
        if (GetRegionData(rgn, size, data) && data->rdh.nCount <= WINE_DCOMP_LAYER_MAX_RECTS)
        {
            const RECT *r = (const RECT *)data->Buffer;

            for (i = 0; i < data->rdh.nCount; ++i)
            {
                RECT c;

                if (IntersectRect(&c, &r[i], clip))
                    out[n++] = c;
            }
            free(data);
            return n;
        }
        free(data);
    }

    /* Too many rectangles, or the region data could not be had: one box. */
    {
        /* The bounding box covers area the region did not, so the layer delivers
         * pixels it does not own -- visible as content bleeding into a gap
         * between two covered strips. */
        static unsigned int box_count;

        if (++box_count <= 5 || !(box_count % 200))
            FIXME("Layer region collapsed to its bounding box #%u: more than %u rectangles "
                    "or region data unavailable, box %s.\n",
                    box_count, WINE_DCOMP_LAYER_MAX_RECTS, wine_dbgstr_rect(&box));
    }
    if (!IntersectRect(&out[0], &box, clip))
        return 0;
    return 1;
}

static void dcomp_layer_clear_box(DWORD *bits, unsigned int w, unsigned int h, const RECT *b)
{
    int y, l = b->left, t = b->top, r = b->right, bo = b->bottom;

    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > (int)w) r = w;
    if (bo > (int)h) bo = h;
    for (y = t; y < bo; ++y)
        memset(bits + (SIZE_T)y * w + l, 0, (SIZE_T)(r - l) * sizeof(DWORD));
}

/* Stop the present path from drawing what we no longer maintain: the tree took
 * the window over, or it covers nothing this frame.  The buffers stay, only the
 * published box goes empty. */
static void dcomp_target_retract_layer(struct dcomp_target *target)
{
    struct wine_dcomp_layer *layer = target->layer;

    if (!layer || !target->layer_live)
        return;
    AcquireSRWLockExclusive(&layer->lock);
    SetRectEmpty(&layer->box);
    layer->rect_count = 0;
    ReleaseSRWLockExclusive(&layer->lock);
    target->layer_live = FALSE;
    TRACE("Retracted the layer on hwnd %p.\n", target->hwnd);
}

/* Caller holds device->cs. */
static void dcomp_target_publish_layer(struct dcomp_target *target, const RECT *rc, HRGN rgn)
{
    struct dcomp_visual *root = target->root_visual;
    unsigned int w = rc->right, h = rc->bottom, back, n, i;
    RECT rects[WINE_DCOMP_LAYER_MAX_RECTS];
    struct wine_dcomp_layer *layer;
    struct dcomp_visual *child;
    UINT saved_w, saved_h;
    DWORD *saved_bits;
    RECT clip, box;

    if (!root || !w || !h)
        return;

    if (!(layer = target->layer))
    {
        /* Pick up the one an earlier target on this window left behind: it is
         * deliberately never freed, see dcomp_layer.h. */
        if ((layer = (struct wine_dcomp_layer *)GetPropW(target->hwnd, WINE_DCOMP_LAYER_PROP))
                && (layer->magic != WINE_DCOMP_LAYER_MAGIC || layer->size != sizeof(*layer)))
        {
            /* Left behind by a dcomp built against another layout.  We own the
             * property, so replace it; the old structure stays unfreed like any
             * other -- a reader of its vintage may still hold a pointer. */
            ERR("Replacing the layer on hwnd %p: magic %#lx, size %lu, expected %#x, %Iu "
                    "-- it was published by a dcomp built against another layout.\n",
                    target->hwnd, layer->magic, layer->size,
                    WINE_DCOMP_LAYER_MAGIC, sizeof(*layer));
            layer = NULL;
        }
        if (!layer)
        {
            if (!(layer = calloc(1, sizeof(*layer))))
                return;
            layer->magic = WINE_DCOMP_LAYER_MAGIC;
            layer->size = sizeof(*layer);
            if (!SetPropW(target->hwnd, WINE_DCOMP_LAYER_PROP, (HANDLE)layer))
            {
                WARN("Failed to publish the layer property on hwnd %p.\n", target->hwnd);
                free(layer);
                return;
            }
        }
        target->layer = layer;
    }

    if (!target->layer_buf[0] || target->layer_width != w || target->layer_height != h)
    {
        DWORD *b0 = calloc((SIZE_T)w * h, sizeof(DWORD));
        DWORD *b1 = calloc((SIZE_T)w * h, sizeof(DWORD));

        if (!b0 || !b1)
        {
            free(b0);
            free(b1);
            return;
        }
        /* A reader may still be on the buffers we are about to free. */
        AcquireSRWLockExclusive(&layer->lock);
        layer->bits = NULL;
        SetRectEmpty(&layer->box);
        layer->rect_count = 0;
        ReleaseSRWLockExclusive(&layer->lock);

        free(target->layer_buf[0]);
        free(target->layer_buf[1]);
        target->layer_buf[0] = b0;
        target->layer_buf[1] = b1;
        target->layer_dirty_n[0] = target->layer_dirty_n[1] = 0;
        target->layer_back = 0;
        target->layer_width = w;
        target->layer_height = h;
    }

    SetRect(&clip, 0, 0, w, h);
    if (!(n = dcomp_layer_region_rects(rgn, rects, &clip)))
    {
        dcomp_target_retract_layer(target);
        return;
    }
    box = rects[0];
    for (i = 1; i < n; ++i)
        UnionRect(&box, &box, &rects[i]);

    back = target->layer_back;

    /* Clear what THIS buffer last carried -- two publications back, not one.
     * Anything left standing there would keep the leaf at a position it has
     * long since left. */
    for (i = 0; i < target->layer_dirty_n[back]; ++i)
        dcomp_layer_clear_box(target->layer_buf[back], w, h, &target->layer_dirty[back][i]);
    for (i = 0; i < n; ++i)
        dcomp_layer_clear_box(target->layer_buf[back], w, h, &rects[i]);

    /* dcomp_target_composite_leaves() writes to target->comp_bits.  Point the
     * field at the back buffer for the walk rather than duplicating the
     * recursion -- under the same lock the ordinary composite path holds. */
    saved_bits = target->comp_bits;
    saved_w = target->comp_width;
    saved_h = target->comp_height;
    target->comp_bits = target->layer_buf[back];
    target->comp_width = w;
    target->comp_height = h;
    for (child = root->children; child; child = child->next_sibling)
        dcomp_target_composite_leaves(target, child, (int)root->offset_x, (int)root->offset_y);
    target->comp_bits = saved_bits;
    target->comp_width = saved_w;
    target->comp_height = saved_h;

    memcpy(target->layer_dirty[back], rects, n * sizeof(*rects));
    target->layer_dirty_n[back] = n;

    /* Publish.  The exclusive acquire is a handful of assignments long, and it
     * doubles as the barrier that lets the buffer we are handing over now be
     * written again two publications from here: any reader still holding it has
     * to be out before this returns. */
    AcquireSRWLockExclusive(&layer->lock);
    layer->bits = target->layer_buf[back];
    layer->width = w;
    layer->height = h;
    layer->box = box;
    layer->rect_count = n;
    memcpy(layer->rects, rects, n * sizeof(*rects));
    ReleaseSRWLockExclusive(&layer->lock);

    TRACE("Published %u rect(s) in (%ld,%ld)-(%ld,%ld) of a %ux%u layer on hwnd %p.\n",
            n, (long)box.left, (long)box.top, (long)box.right, (long)box.bottom,
            w, h, target->hwnd);

    target->layer_live = TRUE;
    target->layer_back = back ^ 1;
}

/* The region to deliver is passed in rather than read off the target: with the
 * delivery running in a thread of its own (below) the tree walk keeps writing
 * frame_rgn while this runs, so the caller hands over a copy it took under the
 * device lock.  Same object, same order for the in-thread caller. */
static void dcomp_target_deliver_region(struct dcomp_target *target, const RECT *rc, HRGN rgn)
{
    struct dcomp_visual *root = target->root_visual;
    struct dcomp_visual *child;
    RECT box, clip_rc;
    HRGN vacated;
    HDC hdc;

    if (!dcomp_region_delivery() || !rgn)
        return;

    /* Hand back what the last delivery painted and this one does not cover.
     * With save-under the hand-back is first our own action: write the
     * backdrop saved before the last blit back to the window, so the vacated
     * area no longer depends on the application repainting it -- it will not,
     * for the reason it uses DirectComposition at all (issue 196).  The
     * restore runs before the delivery because vacated is disjoint to rgn by
     * construction; nothing later in this function writes there.  InvalidateRgn
     * stays as the correction path for the stale case: the application may
     * have painted underneath us, and then the saved backdrop is outdated.
     * The restore region comes off saved_rgn -- exactly the area save_dc
     * still holds a backdrop for -- and with an empty rgn it is all of it,
     * so a leaf that vanishes is restored by this same line (issue 190's
     * playhead disappearing when the transport stops). */
    if (target->delivered_rgn && (vacated = CreateRectRgn(0, 0, 0, 0)))
    {
        if (CombineRgn(vacated, target->delivered_rgn, rgn, RGN_DIFF) > NULLREGION)
        {
            if (dcomp_save_under() && target->saved_rgn && target->save_dc
                    && IsWindowVisible(target->hwnd) && (hdc = GetDC(target->hwnd)))
            {
                HRGN restore = CreateRectRgn(0, 0, 0, 0);

                if (restore
                        && CombineRgn(restore, target->saved_rgn, rgn, RGN_DIFF) > NULLREGION)
                    dcomp_blt_region(hdc, target->save_dc, restore);
                if (restore)
                    DeleteObject(restore);
                ReleaseDC(target->hwnd, hdc);
            }
            InvalidateRgn(target->hwnd, vacated, FALSE);
        }
        DeleteObject(vacated);
    }

    /* The frame route replaces the window blit (issue 206).  It runs after the
     * hand-back above and not before it: a swapchain appearing or going away
     * switches routes mid-flight, and our last blit must not be left standing
     * on a window we have stopped painting.  What the hand-back does not cover
     * -- the part both routes draw -- needs none, the frame route puts the same
     * pixels in the same place.
     *
     * With this route there is nothing to hand back afterwards, so the
     * bookkeeping goes empty: we do not paint the window at all. */
    if (dcomp_target_use_layer_route(target))
    {
        if (target->delivered_rgn)
            SetRectRgn(target->delivered_rgn, 0, 0, 0, 0);
        if (target->saved_rgn)
            SetRectRgn(target->saved_rgn, 0, 0, 0, 0);

        EnterCriticalSection(&target->device->cs);
        dcomp_target_publish_layer(target, rc, rgn);
        LeaveCriticalSection(&target->device->cs);
        return;
    }

    /* Tree covers nothing this frame: the hand-back above is the whole job. */
    if (GetRgnBox(rgn, &box) <= NULLREGION)
    {
        if (target->delivered_rgn)
            SetRectRgn(target->delivered_rgn, 0, 0, 0, 0);
        if (target->saved_rgn)
            SetRectRgn(target->saved_rgn, 0, 0, 0, 0);
        return;
    }

    if (!IsWindowVisible(target->hwnd))
        return;

    EnterCriticalSection(&target->device->cs);
    dcomp_target_ensure_comp_dc(target, rc->right, rc->bottom);
    if (dcomp_save_under())
    {
        dcomp_target_ensure_save_dc(target, rc->right, rc->bottom);
        if (target->save_dc && target->save_bits && !target->saved_rgn)
            target->saved_rgn = CreateRectRgn(0, 0, 0, 0);
    }
    if (target->comp_dc && target->comp_bits)
    {
        HDC hdc_win = GetDC(target->hwnd);

        /* Backdrop for the blend, but only underneath the leaves: a leaf that
         * is not fully opaque has to show what the window has there.  The
         * read-back that issue 187 measured as damaging is this same call over
         * the whole client area; bounded to the leaf rectangles it cannot get
         * in the way of anything the application paints elsewhere.
         *
         * With save-under the backdrop comes out of the saved copy, not the
         * window: where this delivery overlaps the last one the window
         * already carries our own line, and reading it back would bake that
         * line into the background -- a fixed trail the moment the leaf moves
         * on.  The part this delivery newly claims (fresh) has never been
         * painted by us, so the window is the right source for exactly that
         * part, captured into save_dc before the blend touches comp_dc. */
        if (hdc_win)
        {
            if (target->save_dc && target->save_bits && target->saved_rgn)
            {
                HRGN fresh = CreateRectRgn(0, 0, 0, 0);

                if (fresh)
                {
                    if (CombineRgn(fresh, rgn, target->saved_rgn, RGN_DIFF) > NULLREGION)
                        dcomp_blt_region(target->save_dc, hdc_win, fresh);
                    DeleteObject(fresh);
                }
                dcomp_blt_region(target->comp_dc, target->save_dc, rgn);
            }
            else
                dcomp_blt_region(target->comp_dc, hdc_win, rgn);
            ReleaseDC(target->hwnd, hdc_win);
        }

        for (child = root->children; child; child = child->next_sibling)
            dcomp_target_composite_leaves(target, child,
                    (int)root->offset_x, (int)root->offset_y);
    }
    LeaveCriticalSection(&target->device->cs);

    if (!target->comp_dc || !(hdc = GetDC(target->hwnd)))
        return;

    /* An empty clip means the blit cannot reach the window -- BitBlt returns
     * TRUE all the same, so recording a delivery here would invent a hand-back
     * for the next frame. */
    if (GetClipBox(hdc, &clip_rc) > NULLREGION
            && dcomp_blt_region(hdc, target->comp_dc, rgn))
    {
        /* Push the blit out to the display server now, do not leave it queued
         * (issue 206).
         *
         * The application presents through GL, which writes the window
         * immediately.  Our delivery is a batch of GDI operations that turn into
         * XRender composites -- they arrive at the server, but on the server's
         * schedule.  Whenever a swap lands between our composite being queued and
         * being executed, the frame shows the window without our leaves: exactly
         * the playhead flicker.
         *
         * It surfaced by accident: a diagnostic probe reading one pixel back
         * after each delivery -- a synchronous XGetImage, i.e. a round trip that
         * drains the queue as a side effect -- visibly removed almost all of the
         * flicker.  That side effect is the fix.
         *
         * WINE_DCOMP_DELIVER_FLUSH picks how (see dcomp_deliver_flush above for
         * the measurements); 0 turns it off. */
        switch (dcomp_deliver_flush())
        {
        case 1:
            /* Cheapest that works: ask winex11 to push its Xlib queue out.  No
             * image is read and nothing is waited for -- the requests just stop
             * sitting in the client-side buffer. */
            {
                /* Kept in sync with dlls/winex11.drv/x11drv.h. */
                enum { X11DRV_ESCAPE = 6789, X11DRV_FLUSH_DISPLAY = 4 };
                DWORD code = X11DRV_FLUSH_DISPLAY;

                ExtEscape(hdc, X11DRV_ESCAPE, sizeof(code), (const char *)&code, 0, NULL);
            }
            break;
        case 2:
            /* Full round trip -- drains the queue AND waits for the server.
             * Measurably effective, but reads back a pixel we do not need. */
            GetPixel(hdc, clip_rc.left, clip_rc.top);
            break;
        case 3:
            GdiFlush();   /* GDI batch only -- measured as NOT sufficient */
            break;
        case 4:
            /* XSync: drains the queue AND waits for the server, like the round
             * trip above, but transfers no image. */
            {
                enum { X11DRV_ESCAPE = 6789, X11DRV_SYNC_DISPLAY = 5 };
                DWORD code = X11DRV_SYNC_DISPLAY;

                ExtEscape(hdc, X11DRV_ESCAPE, sizeof(code), (const char *)&code, 0, NULL);
            }
            break;
        }

        if (!target->delivered_rgn)
            target->delivered_rgn = CreateRectRgn(0, 0, 0, 0);
        if (target->delivered_rgn)
            CombineRgn(target->delivered_rgn, rgn, NULL, RGN_COPY);

        /* The blit reached the window, and save_dc now holds a valid backdrop
         * for exactly rgn: fresh was captured from the window above, the rest
         * kept the copy from the delivery before.  Only record it when the
         * save-under buffer actually exists -- without it there is nothing to
         * restore from, and saved_rgn must not claim otherwise. */
        if (target->save_dc && target->save_bits)
        {
            if (!target->saved_rgn)
                target->saved_rgn = CreateRectRgn(0, 0, 0, 0);
            if (target->saved_rgn)
                CombineRgn(target->saved_rgn, rgn, NULL, RGN_COPY);
        }

        TRACE("Delivered (%ld,%ld)-(%ld,%ld) of %ldx%ld to hwnd %p, window not claimed.\n",
                (long)box.left, (long)box.top, (long)box.right, (long)box.bottom,
                (long)rc->right, (long)rc->bottom, target->hwnd);
    }
    ReleaseDC(target->hwnd, hdc);
}

/* -------------------------------------------------------------------------
 * Delivering from a thread of our own (issue 190).
 *
 * Everything above runs in the application's GUI thread: the composite is
 * driven by DCOMP_TREE_TIMER through the subclassed wndproc, so the read-back,
 * the blend and the blit-out happen between two of the application's own
 * messages.  That thread is also the one that renders through D2D1 and waits on
 * the wined3d cs thread, so our X traffic and its GL traffic take turns inside
 * one message loop -- and whatever the delivery costs, the application's own
 * painting waits for it.
 *
 * The delivery does not need that thread.  It reads a region the tree walk
 * produced and writes it back to the window; both are legal from any thread.
 * Moving it off means the GUI thread only sets an event, and a thread of ours
 * may block on X as long as it likes without the application's message loop
 * noticing.
 *
 * Only the below-threshold path moves.  A tree that covers its window
 * (covers_window, WebView2 and everything like it) keeps every line of the code
 * it had, in the thread it had it in: that path answers WM_PAINT and must stay
 * ordered against it.
 *
 * That is off by default, and the thread is kept only as a switch.  It was
 * introduced against a freeze that turned out to be an instrumented win32u.so
 * and was never reproducible afterwards, so it has no defect left to pay for --
 * while it does cost: a thread whose lifetime has to be managed across window
 * teardown, a region copy under the device lock, and a second thread reaching
 * for the same composition DC as the GUI thread.
 *
 * Measured against it as well.  On a software-rendered display the line is
 * missing from far more capture frames with the thread than without (median of
 * five runs each, 77.0% against 41.7%, ranges apart).  On a GPU the difference
 * in flicker is not apparent -- but dragging a selection in Fender Studio Pro 8
 * faults with the thread and does not without it, repeatedly and either speed.
 *
 * WINE_DCOMP_DELIVER_THREAD=1 puts the delivery back on a thread of ours, so
 * anyone who does hit a freeze here still has the switch. */
static int dcomp_deliver_thread_enabled = -1;

static BOOL dcomp_deliver_thread(void)
{
    if (dcomp_deliver_thread_enabled < 0)
    {
        const char *e = getenv("WINE_DCOMP_DELIVER_THREAD");
        dcomp_deliver_thread_enabled = (e && atoi(e)) ? 1 : 0;
    }
    return dcomp_deliver_thread_enabled > 0;
}

static DWORD CALLBACK dcomp_target_deliver_proc(void *arg)
{
    struct dcomp_target *target = arg;

    for (;;)
    {
        RECT rc;
        BOOL have, taken_over;

        /* The timeout is a backstop, not the drive: a signal that arrives while
         * a delivery runs is not lost (auto-reset event stays set), and one that
         * arrives after the last frame still gets a pass. */
        WaitForSingleObject(target->deliver_event, 100);
        if (InterlockedCompareExchange(&target->deliver_stop, 0, 0))
            break;

        if (!target->device || !target->hwnd || !IsWindow(target->hwnd))
            continue;
        GetClientRect(target->hwnd, &rc);
        if (rc.right <= 0 || rc.bottom <= 0)
            continue;

        /* Take a copy of this frame's region under the device lock -- the tree
         * walk in the GUI thread keeps rewriting frame_rgn. */
        have = taken_over = FALSE;
        EnterCriticalSection(&target->device->cs);
        /* The takeover latch can flip while we run (issue 187): from then on the
         * GUI thread composites the whole window through the same comp_dc, and a
         * delivery of ours would be a second thread writing it -- for a window
         * that path is already painting whole.  The latch never goes back, so
         * there is nothing left for this thread to do.  The backstop timeout
         * above means it would keep delivering on its own otherwise, long after
         * the composite path stopped asking. */
        if (target->covers_window)
            taken_over = TRUE;
        else if (target->frame_rgn)
        {
            if (!target->deliver_rgn)
                target->deliver_rgn = CreateRectRgn(0, 0, 0, 0);
            if (target->deliver_rgn)
                have = CombineRgn(target->deliver_rgn, target->frame_rgn, NULL, RGN_COPY) != ERROR;
        }
        LeaveCriticalSection(&target->device->cs);

        if (taken_over)
        {
            /* From here the composite path paints the whole window itself.  A
             * layer left published would be drawn on top of it every present,
             * one frame of a tree that has moved on since. */
            dcomp_target_retract_layer(target);
            break;
        }
        if (have)
            dcomp_target_deliver_region(target, &rc, target->deliver_rgn);
    }
    return 0;
}

static BOOL dcomp_target_start_deliver_thread(struct dcomp_target *target)
{
    if (target->deliver_thread)
        return TRUE;
    if (InterlockedCompareExchange(&target->deliver_stop, 0, 0))
        return FALSE;

    if (!target->deliver_event
            && !(target->deliver_event = CreateEventW(NULL, FALSE, FALSE, NULL)))
        return FALSE;
    if (!(target->deliver_thread = CreateThread(NULL, 0, dcomp_target_deliver_proc, target, 0, NULL)))
        return FALSE;

    FIXME("Delivering the covered region of target %p hwnd %p from a thread of our own.\n",
            target, target->hwnd);
    return TRUE;
}

/* Signal only -- never wait here.  Called from the window's own teardown, where
 * blocking on a thread that may be inside a GDI call on that very window would
 * be the freeze we are trying to keep out of the GUI thread. */
static void dcomp_target_stop_deliver_thread(struct dcomp_target *target)
{
    InterlockedExchange(&target->deliver_stop, 1);
    if (target->deliver_event)
        SetEvent(target->deliver_event);
}

/* The join belongs to Release, the one place that frees the target the thread
 * dereferences. */
static void dcomp_target_join_deliver_thread(struct dcomp_target *target)
{
    dcomp_target_stop_deliver_thread(target);
    if (target->deliver_thread)
    {
        WaitForSingleObject(target->deliver_thread, INFINITE);
        CloseHandle(target->deliver_thread);
        target->deliver_thread = NULL;
    }
    if (target->deliver_event)
    {
        CloseHandle(target->deliver_event);
        target->deliver_event = NULL;
    }
    if (target->deliver_rgn)
    {
        DeleteObject(target->deliver_rgn);
        target->deliver_rgn = NULL;
    }
}

static void dcomp_target_composite_tree(struct dcomp_target *target, BOOL from_timer)
{
    struct dcomp_visual *root;
    struct dcomp_visual *child;
    RECT rc;
    HDC hdc;

    if (!target->device || !target->hwnd || !IsWindow(target->hwnd))
        return;
    root = target->root_visual;
    /* Content-bearing roots composite their children via their own Present —
     * this path is only for rootless trees (Chromium/WebView2). */
    if (!root || root->content || root->surface_content)
        return;

    /* Nothing to compose (issue 184): leave before touching a DC.  Every pass
     * here costs a GetDC, a full-window BitBlt readback, the composite walk and
     * a full-window BitBlt back — driven at DCOMP_TREE_TIMER_MS this repaints
     * the window ~60 times a second with the window's own pixels, which is
     * visible as fast flicker on a window the application is presenting to. */
    if (!dcomp_target_tree_carries_content(target))
        return;

    GetClientRect(target->hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0)
        return;

    EnterCriticalSection(&target->device->cs);
    dcomp_target_update_covered(target, &rc);
    LeaveCriticalSection(&target->device->cs);

    /* Too little of the window to call our composition its content (issue 187).
     * Then the honest answer is to keep off it altogether: the read-back and
     * blit below would put a foreign body into a window the application paints
     * itself, and every pass of it fights the application's own drawing.
     * MEASURED on Fender Studio Pro 8: three one-pixel leaves, 0.03% of the
     * window; running this path at all left vertical strips of stale pixels
     * across the arranger that nothing ever repaired, while the same build with
     * the path skipped drew the window exactly like the unpatched one. */
    if (!target->covers_window)
    {
        /* Deliver the covered region all the same, without claiming the window
         * (issue 190): those leaves are content nobody else draws -- Studio
         * Pro 8's entire tree is its transport playhead, and it was missing
         * because below the threshold we composited nothing at all.
         *
         * Hand it to our own thread if we have one: the region for this frame is
         * already in frame_rgn, so all this thread owes the delivery is a signal
         * (see dcomp_target_deliver_proc). */
        if (dcomp_deliver_thread() && dcomp_region_delivery()
                && dcomp_target_start_deliver_thread(target))
            SetEvent(target->deliver_event);
        else
            dcomp_target_deliver_region(target, &rc, target->frame_rgn);
        return;
    }

    /* Taken over: from here we composite the whole window ourselves.  A layer
     * left published from the route below the threshold would be drawn on top
     * of every present after this one, one frame of a tree that has moved on. */
    dcomp_target_retract_layer(target);

    EnterCriticalSection(&target->device->cs);
    dcomp_target_ensure_comp_dc(target, rc.right, rc.bottom);
    if (target->comp_bits)
    {
        static unsigned int comp_tree_log;
        HDC hdc_win = GetDC(target->hwnd);

        /* Unchanged-content gate: seed the walk hash (FNV-1a) with the
         * composition size so a window resize always forces a fresh blit. */
        target->walk_leaf_hash = 2166136261u;
        target->walk_leaf_hash = (target->walk_leaf_hash ^ (DWORD)rc.right) * 16777619u;
        target->walk_leaf_hash = (target->walk_leaf_hash ^ (DWORD)rc.bottom) * 16777619u;
        target->walk_leaf_hash_valid = TRUE;

        /* Capture the live window content as backdrop: transparent page
         * areas must show what is behind (loader artwork / host content),
         * not opaque black (issue 88).
         *
         * The capture fails regularly while the target is being resized — the
         * BitBlt reads the window through X GetImage, which answers BadMatch
         * whenever the window is not fully viewable.  Measured in FL Studio's
         * Hub: 266 failures in one short session, every one of them a black
         * frame on screen (issue 116, proven with a colour marker).
         *
         * Keep the previous backdrop instead: a slightly stale frame is a far
         * better filler than opaque black.  Only clear when no valid backdrop
         * was ever captured at this size, so a freshly allocated DIB still
         * shows black rather than uninitialised memory. */
        if (!hdc_win || !BitBlt(target->comp_dc, 0, 0, rc.right, rc.bottom,
                hdc_win, 0, 0, SRCCOPY))
        {
            if (!target->comp_backdrop_valid)
                memset(target->comp_bits, 0, (SIZE_T)rc.right * rc.bottom * sizeof(DWORD));
        }
        else target->comp_backdrop_valid = TRUE;
        if (hdc_win)
            ReleaseDC(target->hwnd, hdc_win);

        /* The backdrop we just read back IS the window's current content, so it
         * also answers the question the leaf hash cannot: does the target still
         * show the frame we delivered?  A sibling window painting over our area
         * leaves it wrong indefinitely, while the unchanged-content gate keeps
         * skipping because our leaves did not change (issue 107: the tab went
         * black when an unrelated FL window was opened or moved).
         *
         * Re-deliver only once the mismatch has persisted past
         * DCOMP_TARGET_REPAIR_MS, so the tab-switch repaint from issue 99 --
         * which resolves within a few hundred ms -- never triggers it. */
        if (target->last_delivered_valid && from_timer)
        {
            DWORD now = GetTickCount();

            if (dcomp_surface_hash(target->comp_bits,
                    (unsigned int)(rc.right * rc.bottom)) == target->last_delivered_hash)
                target->target_diverged_tick = 0;
            else if (!target->target_diverged_tick)
                target->target_diverged_tick = now | 1;
            else if (now - target->target_diverged_tick >= DCOMP_TARGET_REPAIR_MS)
            {
                target->last_blit_leaf_valid = FALSE;
                target->target_diverged_tick = 0;
            }
        }

        for (child = root->children; child; child = child->next_sibling)
            dcomp_target_composite_leaves(target, child,
                    (int)root->offset_x, (int)root->offset_y);

        if (++comp_tree_log <= 5 || !(comp_tree_log % 100))
            FIXME("Composited visual tree #%u onto target hwnd %p (%ldx%ld).\n",
                    comp_tree_log, target->hwnd, (long)rc.right, (long)rc.bottom);
    }
    LeaveCriticalSection(&target->device->cs);

    if (target->comp_dc)
    {
        BOOL skip_same = FALSE, blt_out = FALSE;
        int clip_out = -1;
        RECT clip_out_rc = {0};

        hdc = GetDC(target->hwnd);
        if (hdc)
        {
            /* Paint only as far as the tree reaches (issue 187).  Everything
             * outside comes straight out of the backdrop read back from this
             * very window a moment ago, so writing it back is at best a no-op
             * -- and at worst it loses whatever the application painted in
             * between, sixty times a second.  A tree that covers the window
             * clips to the same area as before and is unaffected. */
            clip_out = GetClipBox(hdc, &clip_out_rc);

            /* Parked targets must not be painted: WebView2 keeps the target
             * window WS_VISIBLE when the host app parks the pane under a
             * hidden helper toplevel (Ableton Live's Learn View close), and
             * the DCE clip region cannot be trusted to go empty — it stays
             * stale for SetParent'ed non-WS_CHILD windows (issue 121,
             * reproduced app-free). IsWindowVisible() walks the real ancestor
             * styles and is not affected, so gate the blit on it; void the
             * delivery record so the first composite after re-exposure paints
             * fresh. Same pattern as the reblit gate from shibco's issue 57. */
            if (!IsWindowVisible(target->hwnd))
            {
                skip_same = TRUE;
                target->last_blit_leaf_valid = FALSE;
            }

            /* Switching to another application can cost us the pixels we
             * blitted into the shared drawable, while none of our source
             * leaves change.  The content hash only proves that our source is
             * unchanged — it cannot prove the target still carries our pixels,
             * so the gate below would skip forever and the tab stays blank
             * until something happens to alter a leaf (issue 115: mouse-over
             * or a tab round-trip was needed to bring the content back).
             * Void the delivery record once per foreground change; a tab
             * switch keeps the same foreground window and is unaffected, so
             * the issue 99 tab-bleed guard keeps working as before. */
            if (from_timer)
            {
                HWND fg = GetForegroundWindow();

                if (fg != target->last_foreground)
                {
                    target->last_foreground = fg;
                    target->last_blit_leaf_valid = FALSE;
                }
            }

            if (from_timer && dcomp_skip_unchanged())
            {
                /* Empty clip: the blit cannot paint anything — skip it, and
                 * above all never record it as delivered (BitBlt still
                 * returns TRUE on a NULLREGION clip). */
                if (clip_out == NULLREGION)
                    skip_same = TRUE;
                /* Skip only when nothing changed since the last blit that
                 * really painted: same leaf content AND same clip box — a
                 * grown clip exposes area the last blit never covered.
                 * SIMPLEREGION only: with a complex region the bounding box
                 * cannot prove the visible area is unchanged. */
                else if (clip_out == SIMPLEREGION
                        && target->walk_leaf_hash_valid && target->last_blit_leaf_valid
                        && target->walk_leaf_hash == target->last_blit_leaf_hash
                        && EqualRect(&clip_out_rc, &target->last_blit_clip_rc))
                    skip_same = TRUE;
            }

            if (!skip_same)
                blt_out = BitBlt(hdc, 0, 0, rc.right, rc.bottom, target->comp_dc, 0, 0, SRCCOPY);
            else
                TRACE("issue-99 skip-unchanged: target %p hwnd %p %ux%u clip %d.\n",
                        target, target->hwnd, target->comp_width, target->comp_height, clip_out);
            ReleaseDC(target->hwnd, hdc);
        }

        /* Record delivery only when pixels could actually reach the window. */
        if (blt_out && clip_out >= SIMPLEREGION)
        {
            target->last_blit_leaf_hash = target->walk_leaf_hash;
            target->last_blit_leaf_valid = target->walk_leaf_hash_valid;
            target->last_blit_clip_rc = clip_out_rc;
            /* Remember what the window should show from now on, so the next
             * backdrop readback can tell whether it still does. */
            if (target->comp_bits)
            {
                target->last_delivered_hash = dcomp_surface_hash(target->comp_bits,
                        (unsigned int)(rc.right * rc.bottom));
                target->last_delivered_valid = TRUE;
            }
            target->target_diverged_tick = 0;
        }
        else if (clip_out < SIMPLEREGION)
        {
            /* Empty or unknown clip: while we could not paint, a sibling
             * WebView tab may have painted the shared drawable over our
             * pixels — the delivery record is void, the first composite
             * after re-exposure must blit again even with unchanged leaves
             * (hub windows where every tab is a static WebView page). */
            target->last_blit_leaf_valid = FALSE;
        }

        /* Hand the area back to the host exactly once per visible->hidden
         * transition — the pixels we blitted stay behind in the shared
         * drawable and the host never repaints a "self-drawing" child. */
        if (clip_out >= 0 && clip_out != target->hide_restore_clip)
        {
            if (clip_out == NULLREGION && target->hide_restore_clip != NULLREGION
                    && dcomp_host_restore())
            {
                RECT wr;

                GetWindowRect(target->hwnd, &wr);
                dcomp_restore_area_to_host(target, &wr, "on-hide");
            }
            target->hide_restore_clip = clip_out;
        }

        /* The hub sidebar resizes per tab (right-anchored: the left edge
         * moves).  Pixels delivered at the old geometry stay behind in the
         * shared drawable — hand the vacated remainder back to the host. */
        if (dcomp_host_restore())
        {
            HWND top = GetAncestor(target->hwnd, GA_ROOT);
            RECT wr, tr = {0};

            GetWindowRect(target->hwnd, &wr);
            if (top)
                GetWindowRect(top, &tr);
            if (target->last_blit_win_valid && !EqualRect(&wr, &target->last_blit_win_rect))
            {
                const RECT *last = &target->last_blit_win_rect;
                BOOL resized = (wr.right - wr.left) != (last->right - last->left)
                        || (wr.bottom - wr.top) != (last->bottom - last->top);
                BOOL top_moved = !EqualRect(&tr, &target->last_blit_top_rect);
                RECT vacated;

                /* Only area we truly vacated relative to the host may be handed
                 * back.  Dragging the window moves toplevel and target together:
                 * our screen rect changes at every 100 ms tick, yet nothing is
                 * left behind — the host travels along.  Restoring then runs
                 * RDW_ERASE over the live composition ten times a second, and
                 * the class brush paints it white; that is the flicker seen
                 * while moving a plugin editor.  A size change (the hub sidebar
                 * case that motivated the restore) or a target that moved while
                 * its toplevel stood still is a genuine vacate and still
                 * restores.
                 *
                 * SubtractRect keeps the whole old rect when the difference is
                 * not a single rectangle — a safe superset of the vacated area. */
                if ((resized || !top_moved)
                        && SubtractRect(&vacated, &target->last_blit_win_rect, &wr))
                    dcomp_restore_area_to_host(target, &vacated, "vacated");
                /* Keep tracking at the new geometry even when this composite
                 * delivers nothing (empty clip / unchanged skip): multi-step
                 * resizes must hand back every intermediate footprint.
                 * Over-restoring costs one host repaint; under-restoring
                 * leaks the stale frame. */
                target->last_blit_win_rect = wr;
                target->last_blit_top_rect = tr;
            }
            if (blt_out && clip_out >= SIMPLEREGION)
            {
                target->last_blit_win_rect = wr;
                target->last_blit_top_rect = tr;
                target->last_blit_win_valid = TRUE;
            }
        }
    }
}

#define DCOMP_COALESCE_TIMER  ((UINT_PTR)0xDC0FFEE1)
#define DCOMP_FRAME_MS        15u   /* ~66 Hz present cap (issue 56) */

/* Read back region [l,r)x[t,b) of the GPU render target into surface->bits.  Split out of
 * EndDraw and deferred to the throttled present (issue 56): the GPU→CPU Map forces a
 * ~1.3ms GPU sync stall, so running it once per present (~60/s) instead of once per
 * EndDraw (~500/s during a fast drag) keeps the render thread free.  D2D command
 * finalization (ID2D1DeviceContext_EndDraw) already ran in dcomp_surface_EndDraw. */
static void dcomp_surface_readback_region(struct dcomp_surface *surface,
        LONG l, LONG t, LONG r, LONG b)
{
    unsigned int y, copy_w;
    HRESULT hr;

    if (r <= l || b <= t)
        return;
    copy_w = (unsigned int)(r - l);

    if (surface->d2d1_device && surface->persistent_context && surface->readback_bitmap)
    {
        D2D1_MAPPED_RECT d2d_mapped;
        D2D1_POINT_2U dest_point;
        D2D1_RECT_U src_rect;

        dest_point.x = (UINT32)l;
        dest_point.y = (UINT32)t;
        src_rect.left = (UINT32)l;   src_rect.top    = (UINT32)t;
        src_rect.right = (UINT32)r;  src_rect.bottom = (UINT32)b;

        hr = ID2D1Bitmap1_CopyFromBitmap(surface->readback_bitmap, &dest_point,
                (ID2D1Bitmap *)surface->target_bitmap, &src_rect);
        if (FAILED(hr))
        {
            FIXME("CopyFromBitmap failed: %#lx.\n", hr);
            return;
        }

        hr = ID2D1Bitmap1_Map(surface->readback_bitmap, D2D1_MAP_OPTIONS_READ, &d2d_mapped);
        if (SUCCEEDED(hr))
        {
            for (y = (unsigned int)t; y < (unsigned int)b; ++y)
                memcpy(surface->bits + y * surface->width + l,
                        (const BYTE *)d2d_mapped.bits + y * d2d_mapped.pitch + l * sizeof(DWORD),
                        copy_w * sizeof(DWORD));
            ID2D1Bitmap1_Unmap(surface->readback_bitmap);
        }
        else
            FIXME("ID2D1Bitmap1_Map failed: %#lx.\n", hr);
    }
    else if (surface->staging && surface->texture && surface->bits)
    {
        ID3D11DeviceContext *d3d_context;
        D3D11_MAPPED_SUBRESOURCE mapped;
        D3D11_BOX src_box;

        ID3D11Device_GetImmediateContext(surface->d3d11_device, &d3d_context);
        src_box.left = (UINT)l;  src_box.top = (UINT)t;
        src_box.right = (UINT)r; src_box.bottom = (UINT)b;
        src_box.front = 0; src_box.back = 1;
        ID3D11DeviceContext_CopySubresourceRegion(d3d_context,
                (ID3D11Resource *)surface->staging, 0, (UINT)l, (UINT)t, 0,
                (ID3D11Resource *)surface->texture, 0, &src_box);

        hr = ID3D11DeviceContext_Map(d3d_context, (ID3D11Resource *)surface->staging, 0,
                D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(hr))
        {
            for (y = (unsigned int)t; y < (unsigned int)b; ++y)
                memcpy(surface->bits + y * surface->width + l,
                        (const BYTE *)mapped.pData + y * mapped.RowPitch + l * sizeof(DWORD),
                        copy_w * sizeof(DWORD));
            ID3D11DeviceContext_Unmap(d3d_context, (ID3D11Resource *)surface->staging, 0);
        }
        else
            FIXME("Failed to map staging texture: %#lx.\n", hr);
        ID3D11DeviceContext_Release(d3d_context);
    }
    surface->needs_full_init_copy = FALSE;
}

static LONGLONG dcomp_qpc_now(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

static LONGLONG dcomp_qpc_freq(void)
{
    static LONGLONG freq;
    if (!freq)
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        freq = f.QuadPart ? f.QuadPart : 1;
    }
    return freq;
}

/* Present region [left,right)x[top,bottom) of the target's root surface to its HWND.
 * Caller holds device->cs and has validated root_visual/surface->bits/comp_bits. */
static void dcomp_target_present_region(struct dcomp_target *target,
        struct dcomp_surface *surface, LONG left, LONG top, LONG right, LONG bottom,
        BOOL present_dirty)
{
    static unsigned int present_log;
    HDC hdc;

    /* Copy surface bits → DIB.  Dirty rows only when present_dirty; the rest of
     * comp_bits stays valid from the previous frame (persistent DIB section). */
    if (present_dirty)
    {
        LONG yy;
        for (yy = top; yy < bottom; yy++)
            memcpy((DWORD *)target->comp_bits + yy * surface->width + left,
                    (DWORD *)surface->bits + yy * surface->width + left,
                    (right - left) * sizeof(DWORD));
    }
    else
    {
        memcpy(target->comp_bits, surface->bits,
                surface->width * surface->height * sizeof(DWORD));
    }

    /* Composite child visual surfaces on top (premultiplied alpha over) */
    {
        struct dcomp_visual *child;
        static unsigned int child_comp_log;

        for (child = target->root_visual->children; child; child = child->next_sibling)
        {
            struct dcomp_surface *child_surf = child->surface_content;
            int ox, oy, src_x, src_y, dst_x, dst_y, copy_w, copy_h, y, x;
            DWORD *dst_row, *src_row;

            if (!child_surf || !child_surf->bits || !child_surf->width || !child_surf->height)
                continue;

            ox = (int)child->offset_x;
            oy = (int)child->offset_y;

            src_x = (ox < 0) ? -ox : 0;
            src_y = (oy < 0) ? -oy : 0;
            dst_x = (ox < 0) ? 0 : ox;
            dst_y = (oy < 0) ? 0 : oy;
            copy_w = min((int)child_surf->width - src_x, (int)surface->width - dst_x);
            copy_h = min((int)child_surf->height - src_y, (int)surface->height - dst_y);

            if (copy_w <= 0 || copy_h <= 0)
                continue;

            if (++child_comp_log <= 5)
                FIXME("Compositing child visual %ux%u at (%d,%d) onto %ux%u root.\n",
                        child_surf->width, child_surf->height, ox, oy,
                        surface->width, surface->height);

            for (y = 0; y < copy_h; y++)
            {
                dst_row = (DWORD *)target->comp_bits + (dst_y + y) * surface->width + dst_x;
                src_row = (DWORD *)child_surf->bits + (src_y + y) * child_surf->width + src_x;
                for (x = 0; x < copy_w; x++)
                {
                    DWORD s = src_row[x];
                    BYTE sa = (s >> 24);
                    if (sa == 0xff)
                    {
                        dst_row[x] = s;
                    }
                    else if (sa > 0)
                    {
                        /* Premultiplied alpha: out = src + dst * (1 - sa/255) */
                        DWORD d = dst_row[x];
                        BYTE ia = 255 - sa;
                        dst_row[x] = ((min(sa + (((d >> 24) * ia + 127) / 255), 255u)) << 24)
                                   | ((((s >> 16) & 0xff) + ((((d >> 16) & 0xff) * ia + 127) / 255)) << 16)
                                   | ((((s >> 8) & 0xff) + ((((d >> 8) & 0xff) * ia + 127) / 255)) << 8)
                                   | (((s & 0xff) + (((d & 0xff) * ia + 127) / 255)));
                    }
                }
            }
        }
    }

    /* BitBlt only the present region to the target window (the rest already shows the
     * prior frame — WM_ERASEBKGND is suppressed). */
    hdc = GetDC(target->hwnd);
    if (hdc)
    {
        BitBlt(hdc, left, top, right - left, bottom - top,
                target->comp_dc, left, top, SRCCOPY);
        ReleaseDC(target->hwnd, hdc);
    }
    target->comp_needs_full_present = FALSE;

    if (++present_log <= 5)
        FIXME("Present: %s %ldx%ld@(%ld,%ld) of %ux%u to hwnd %p%s.\n",
                present_dirty ? "DIRTY" : "FULL",
                right - left, bottom - top, left, top,
                surface->width, surface->height, target->hwnd,
                target->root_visual->children ? " [children]" : "");

    /* Also serialize child visual tree (for mixed surface+swapchain scenarios) */
    dcomp_commit_visual_tree(target->hwnd, target->root_visual);
}

/* Readback the accumulated dirty region GPU→CPU, then present it (issue 56).  Caller holds
 * device->cs and has validated surface->bits/comp_bits.  Shared by the throttled commit and
 * the WM_TIMER trailing flush. */
static void dcomp_target_flush_present(struct dcomp_target *target, struct dcomp_surface *surface)
{
    BOOL present_dirty = surface->has_pending && !target->root_visual->children
                      && !target->comp_needs_full_present;
    LONG l, t, r, b;

    if (present_dirty)
    {
        l = surface->pending_dirty.left;  t = surface->pending_dirty.top;
        r = surface->pending_dirty.right; b = surface->pending_dirty.bottom;
        if (l < 0) l = 0;
        if (t < 0) t = 0;
        if (r > (LONG)surface->width)  r = (LONG)surface->width;
        if (b > (LONG)surface->height) b = (LONG)surface->height;
    }
    else
    {
        l = 0; t = 0;
        r = (LONG)surface->width;
        b = (LONG)surface->height;
    }

    if (r <= l || b <= t)
    {
        surface->has_pending = FALSE;
        SetRectEmpty(&surface->pending_dirty);
        return;
    }

    /* Deferred GPU→CPU readback of the accumulated region — the expensive part (~1.3ms),
     * now run once per present instead of once per EndDraw. */
    if (surface->has_pending)
        dcomp_surface_readback_region(surface, l, t, r, b);
    surface->has_pending = FALSE;
    SetRectEmpty(&surface->pending_dirty);

    dcomp_target_present_region(target, surface, l, t, r, b, present_dirty);
    target->comp_needs_full_present = FALSE;
    target->last_present_qpc = dcomp_qpc_now();
}

/* Phase 5: WndProc subclass — suppresses WM_ERASEBKGND to prevent white-on-open.
 * WM_PAINT is forwarded to the original WndProc so that VSTGUI can call
 * BeginDraw → Render → EndDraw → Commit in response to paint requests. */
static LRESULT CALLBACK dcomp_target_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    struct dcomp_target *target = (struct dcomp_target *)GetPropW(hwnd, dcomp_target_prop);
    WNDPROC orig_wndproc;

    if (!target)
    {
        /* Orphaned subclass: the last target for this window is gone — its
         * Release drops the property, but cannot unhook while e.g. dxgi is
         * subclassed on top of us (its procedure keeps calling ours as the
         * saved "original").  Swallowing into DefWindowProc here drops the
         * application's own procedure out of the chain: the window keeps
         * painting (the composite path runs independently of the WndProc)
         * but all input is dead.  Forward to the application's procedure
         * preserved for the window's lifetime instead. */
        WNDPROC real = (WNDPROC)GetPropW(hwnd, dcomp_real_wndproc_prop);

        if (real && real != dcomp_target_wndproc)
        {
            static unsigned int orphan_forward_count;

            if (++orphan_forward_count <= 3 || !(orphan_forward_count % 1000))
                FIXME("Forwarding message %#x for orphaned subclass on hwnd %p to %p #%u.\n",
                        msg, hwnd, real, orphan_forward_count);
            return CallWindowProcW(real, hwnd, msg, wparam, lparam);
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    orig_wndproc = target->orig_wndproc;

    switch (msg)
    {
    case WM_ERASEBKGND:
        /* Suppress background erasure — our BitBlt content should persist.
         * Without this, the white class brush overwrites the composed frame
         * after window events (resize, show, focus change) → white-on-open. */
        return 1;

    case WM_TIMER:
        /* Coalescing trailing flush (issue 56): a present was throttled within the ~60 Hz
         * window; this fires once the UI thread goes idle (drag pause/end) and flushes the
         * accumulated dirty region (readback + present) so the final frame is never stale. */
        if (wparam == DCOMP_COALESCE_TIMER)
        {
            KillTimer(hwnd, DCOMP_COALESCE_TIMER);
            if (target->device && target->root_visual && target->root_visual->surface_content)
            {
                struct dcomp_surface *surface = target->root_visual->surface_content;
                EnterCriticalSection(&target->device->cs);
                if (surface->has_pending && surface->bits && surface->width
                        && surface->height && target->comp_bits)
                    dcomp_target_flush_present(target, surface);
                LeaveCriticalSection(&target->device->cs);
            }
            return 0;
        }
        /* Rootless-tree compositing (issue 88): the leaves present into their
         * hidden comp windows independently; we re-compose them onto the
         * target window periodically. */
        if (wparam == DCOMP_TREE_TIMER)
        {
            dcomp_target_composite_tree(target, TRUE);
            return 0;
        }
        break;

    case WM_WINE_DCOMP_PRESENT_FLUSH:
        /* A present has just overwritten the window with its backbuffer
         * (issue 206): re-deliver what our tree owes this window NOW instead
         * of at the next tree timer tick.  dxgi only posts this message on
         * windows carrying __wine_dcomp_present_flush, so reaching here means
         * the flush applies.  Posted, not sent: the blit runs on the thread
         * that owns the window (this one), never on the presenting thread --
         * the cross-thread delivery of issue 190 dragged selections in Studio
         * Pro down.  Disabled, the message falls through to the original
         * wndproc, which restores pure timer behaviour. */
        if (dcomp_present_driven())
        {
            dcomp_target_composite_tree(target, FALSE);
            return 0;
        }
        break;

    case WM_PAINT:
        /* Rootless tree (issue 88): repaint from the composed tree instead of
         * the original wndproc (which would show the empty/loader content).
         * Content-bearing roots keep the VSTGUI paint path.
         *
         * Only take the message away from the application while our tree has
         * something to deliver in its place (issue 184) — otherwise the window
         * is validated by a composition of nothing and the application, never
         * asked to redraw again, freezes on its last frame. */
        if (target->root_visual && !target->root_visual->content
                && !target->root_visual->surface_content
                && dcomp_target_tree_carries_content(target))
        {
            PAINTSTRUCT ps;

            /* Also brings covered_rgn and covers_window up to date. */
            dcomp_target_composite_tree(target, FALSE);

            /* A tree that covers too little of the window never took it over,
             * so its WM_PAINT was never ours to answer (issue 187).  Let the
             * application answer it in full -- and put our leaves back on top
             * afterwards (issue 190), because its BeginPaint clip may well have
             * covered them.  Waiting for the next tree timer tick instead would
             * drop the line for up to a frame every time the application
             * repaints that area, which while scrolling is constantly. */
            if (!target->covers_window)
            {
                LRESULT ret = orig_wndproc
                        ? CallWindowProcW(orig_wndproc, hwnd, msg, wparam, lparam)
                        : DefWindowProcW(hwnd, msg, wparam, lparam);

                dcomp_target_composite_tree(target, FALSE);
                return ret;
            }

            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }

        /* Surface-bearing root: the application paints into the composition
         * surface and treats it as retained -- on Windows the DWM keeps
         * composing it no matter what the window's own redirection surface
         * shows.  Here the surface reaches the window through a BitBlt at
         * present time, and a present that lands before the window is shown
         * goes into win32u's dummy surface.  The window then comes up with the
         * surface it got at ShowWindow, and the application, asked to repaint,
         * repaints nothing: from its side there is nothing to repaint.  So
         * answer its WM_PAINT the way the DWM would -- let it paint, and if
         * that did not present anew, put the last composed frame back for the
         * area the window asked for.  (The Grand 3: the toolbar above the
         * plug-in view stayed black until a hover repainted it piecewise.) */
        if (target->root_visual && target->root_visual->surface_content
                && target->comp_bits && target->last_present_qpc)
        {
            LONGLONG present_before = target->last_present_qpc;
            RECT update;
            LRESULT ret;

            if (!GetUpdateRect(hwnd, &update, FALSE))
                break;

            ret = orig_wndproc
                    ? CallWindowProcW(orig_wndproc, hwnd, msg, wparam, lparam)
                    : DefWindowProcW(hwnd, msg, wparam, lparam);

            if (target->device && target->last_present_qpc == present_before)
            {
                EnterCriticalSection(&target->device->cs);
                /* A frame that is drawn but still waiting for the coalesced
                 * present (issue 56) reaches the window through that present;
                 * only a window with nothing on the way needs the old frame
                 * back. */
                if (target->comp_dc && target->comp_bits
                        && target->root_visual && target->root_visual->surface_content
                        && !target->root_visual->surface_content->has_pending)
                {
                    HDC hdc;

                    if (update.left < 0) update.left = 0;
                    if (update.top < 0) update.top = 0;
                    if (update.right > (LONG)target->comp_width) update.right = target->comp_width;
                    if (update.bottom > (LONG)target->comp_height) update.bottom = target->comp_height;
                    if (update.right > update.left && update.bottom > update.top
                            && (hdc = GetDC(hwnd)))
                    {
                        TRACE("Restoring %s of the composed frame on hwnd %p after WM_PAINT.\n",
                                wine_dbgstr_rect(&update), hwnd);
                        BitBlt(hdc, update.left, update.top, update.right - update.left,
                                update.bottom - update.top, target->comp_dc,
                                update.left, update.top, SRCCOPY);
                        ReleaseDC(hwnd, hdc);
                    }
                }
                LeaveCriticalSection(&target->device->cs);
            }
            return ret;
        }
        break;

    case WM_NCDESTROY:
        /* Window is being destroyed — clean up subclass and forward.  A target
         * that inherited the subclass instead of installing it has no original
         * wndproc of its own; restoring NULL would leave the window without a
         * procedure at all. */
        KillTimer(hwnd, DCOMP_TREE_TIMER);
        /* Tell our delivery thread to stop, but do not wait for it here: it may
         * be inside a GDI call on the very window being torn down, and blocking
         * the GUI thread on that is exactly what this thread exists to avoid.
         * Release joins it (issue 190). */
        dcomp_target_stop_deliver_thread(target);
        if (orig_wndproc)
        {
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)orig_wndproc);
            RemovePropW(hwnd, dcomp_subclass_proc_prop);
        }
        RemovePropW(hwnd, dcomp_target_prop);
        RemovePropW(hwnd, dcomp_present_flush_prop);
        target->orig_wndproc = NULL;
        if (!orig_wndproc)
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        return CallWindowProcW(orig_wndproc, hwnd, msg, wparam, lparam);
    }

    if (!orig_wndproc)
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    return CallWindowProcW(orig_wndproc, hwnd, msg, wparam, lparam);
}

static HRESULT STDMETHODCALLTYPE dcomp_device_Commit(IDCompositionDevice *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice(iface);
    struct dcomp_target *target;

    /* Hold the device lock across iteration so a concurrent CreateTargetForHwnd
     * or target Release cannot mutate the list mid-walk (iterator invalidation /
     * use-after-free).  Recursive (same-thread) re-entry via the auto-commit path
     * is prevented by the in_auto_commit guard, so this does not self-deadlock. */
    EnterCriticalSection(&device->cs);
    for (target = device->targets; target; target = target->next_target)
    {
        struct dcomp_surface *surface;
        LONGLONG now;

        if (!target->root_visual || !target->root_visual->surface_content)
            continue;

        surface = target->root_visual->surface_content;
        if (!surface->bits || !surface->width || !surface->height)
            continue;

        /* Ensure we have a compatible DIB section for presentation */
        dcomp_target_ensure_comp_dc(target, surface->width, surface->height);
        if (!target->comp_bits)
            continue;

        /* Nothing new since the last present and no forced full present pending. */
        if (!surface->has_pending && !target->comp_needs_full_present)
            continue;

        /* Coalesce: cap the present (and its deferred ~1.3ms GPU readback) to ~60 Hz.
         * FL does ~10 EndDraws/frame during a fast drag (~500/s); doing the readback for
         * each starves the render thread (issue 56).  EndDraw already accumulated the dirty
         * region; skipping here defers readback+present to the next slot, with the WM_TIMER
         * trailing flush guaranteeing the final frame.  Forced full presents (first frame /
         * DIB recreate) always go through. */
        now = dcomp_qpc_now();
        if (!target->comp_needs_full_present && target->last_present_qpc
                && (now - target->last_present_qpc) * 1000 / dcomp_qpc_freq() < DCOMP_FRAME_MS)
        {
            SetTimer(target->hwnd, DCOMP_COALESCE_TIMER, DCOMP_FRAME_MS, NULL);
            continue;
        }

        KillTimer(target->hwnd, DCOMP_COALESCE_TIMER);
        dcomp_target_flush_present(target, surface);
    }
    LeaveCriticalSection(&device->cs);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_WaitForCommitCompletion(IDCompositionDevice *iface)
{
    TRACE("iface %p.\n", iface);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_GetFrameStatistics(IDCompositionDevice *iface,
        DCOMPOSITION_FRAME_STATISTICS *statistics)
{
    FIXME("iface %p, statistics %p stub!\n", iface, statistics);

    if (statistics)
        memset(statistics, 0, sizeof(*statistics));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateTargetForHwnd(IDCompositionDevice *iface,
        HWND hwnd, BOOL topmost, IDCompositionTarget **target)
{
    struct dcomp_target *previous;
    struct dcomp_target *object;
    WNDPROC installed;

    TRACE("iface %p, hwnd %p, topmost %d, target %p.\n", iface, hwnd, topmost, target);

    if (!target)
        return E_INVALIDARG;

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->IDCompositionTarget_iface.lpVtbl = &dcomp_target_vtbl;
    object->refcount = 1;
    object->hwnd = hwnd;

    /* Link target into device's list for Commit iteration (under the device lock,
     * since Commit may be iterating the list from another thread). */
    {
        struct dcomp_device *device = impl_from_IDCompositionDevice(iface);
        object->device = device;
        EnterCriticalSection(&device->cs);
        object->next_target = device->targets;
        device->targets = object;
        LeaveCriticalSection(&device->cs);
    }

    /* Phase 5: Subclass target HWND for WM_ERASEBKGND / WM_PAINT protection.
     * The installed WndProc suppresses WM_ERASEBKGND per-window (returns 1), so
     * the class background brush is never painted for this target — we no longer
     * zero the class-wide GCLP_HBRBACKGROUND, which would also affect unrelated
     * windows sharing the class. */
    previous = (struct dcomp_target *)GetPropW(hwnd, dcomp_target_prop);
    SetPropW(hwnd, dcomp_target_prop, (HANDLE)object);

    /* Cross-process targets (e.g. Tauri/WebView2 helper targeting a window
     * owned by the main process, issue 88): do NOT subclass. The installed
     * WndProc would live in this process, so every BeginPaint in the owner
     * process would try to send WM_ERASEBKGND across the process boundary —
     * wineserver cannot pack it (HDC payload) and the owner's paint loop
     * wedges. Foreign targets are composited from the AddVisual/SetContent
     * hooks instead (plain GDI ops work cross-process). */
    {
        DWORD pid = 0;

        GetWindowThreadProcessId(hwnd, &pid);
        if (pid && pid != GetCurrentProcessId())
        {
            object->foreign = TRUE;
            FIXME("Created composition target %p for hwnd %p (foreign process %lu, no subclass).\n",
                    object, hwnd, pid);
            *target = &object->IDCompositionTarget_iface;
            return S_OK;
        }
    }

    /* In-process from here: our wndproc will handle WM_WINE_DCOMP_PRESENT_FLUSH
     * for this window.  Foreign targets never get the property, so dxgi never
     * posts the flush message to a window whose wndproc is not ours.  The
     * property is also what carries the env gate: with present-driven delivery
     * disabled it is never set, so dxgi never posts and no message reaches
     * the application's wndproc -- pure timer behaviour, one gate, one place
     * (issue 206). */
    if (dcomp_present_driven())
        SetPropW(hwnd, dcomp_present_flush_prop, (HANDLE)1);

    /* Never chain a second subclass onto ourselves.  A window can receive another
     * composition target - the app recreates them when the plugin window is
     * resized - and dxgi installs the same wndproc for composition swapchains.
     * Subclassing again would make SetWindowLongPtrW hand back our own wndproc as
     * the "original" one, so CallWindowProcW would call us forever; the
     * WM_NCHITTEST flood of a resize then exhausts the host main thread's 1 MB
     * stack within a few dozen mouse moves and takes the process down.
     *
     * The same applies once dxgi has subclassed on top of us: its procedure
     * still calls ours, so chaining onto it would close the ring the other way
     * round - ours calls dxgi's, dxgi's reads the procedure it saved (ours) and
     * calls back into us.  Our procedure is already in the chain in that case
     * and the new target is picked up through dcomp_target_prop, so there is
     * nothing left to install (issue 101). */
    installed = (WNDPROC)GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    if (installed == dcomp_target_wndproc
            || (installed && installed == (WNDPROC)GetPropW(hwnd, dcomp_subclass_proc_prop)
                && (previous || GetPropW(hwnd, dcomp_real_wndproc_prop))))
    {
        /* Inherit the real wndproc from whoever installed the subclass and clear
         * it there, so its Release neither restores a stale wndproc nor drops the
         * property this target now owns. */
        if (previous && previous != object)
        {
            object->orig_wndproc = previous->orig_wndproc;
            previous->orig_wndproc = NULL;
        }

        /* Fall back to the procedure stashed by whoever subclassed the window
         * first.  Without it the application's wndproc drops out of the chain and
         * the window only ever sees DefWindowProcW: clicks never reach the plugin
         * and nothing repaints. */
        if (!object->orig_wndproc)
            object->orig_wndproc = (WNDPROC)GetPropW(hwnd, dcomp_real_wndproc_prop);
        if (!object->orig_wndproc)
        {
            /* The procedure dxgi saved as the "original" one may well be ours,
             * because it subclassed on top of us; adopting it would make us call
             * ourselves.  Take it only when it is neither our own procedure nor
             * the one currently installed. */
            WNDPROC saved = (WNDPROC)GetPropW(hwnd, L"__wine_dcomp_orig_wndproc");

            if (saved != dcomp_target_wndproc && saved != installed)
                object->orig_wndproc = saved;
        }

        FIXME("Created composition target %p for hwnd %p (already subclassed by %p, inherited orig_wndproc %p).\n",
                object, hwnd, installed, object->orig_wndproc);
    }
    else
    {
        object->orig_wndproc = (WNDPROC)SetWindowLongPtrW(hwnd,
                GWLP_WNDPROC, (LONG_PTR)dcomp_target_wndproc);

        /* Publish what we installed so dxgi recognises the subclass as ours. */
        SetPropW(hwnd, dcomp_subclass_proc_prop, (HANDLE)dcomp_target_wndproc);

        /* Remember the application's own procedure for the whole lifetime of the
         * window.  Targets are recreated on resize, and the one that installed the
         * subclass may be released before its successor appears - without this the
         * real procedure would be lost and the window would fall back to
         * DefWindowProcW.  Set once, never overwritten. */
        if (object->orig_wndproc && !GetPropW(hwnd, dcomp_real_wndproc_prop))
            SetPropW(hwnd, dcomp_real_wndproc_prop, (HANDLE)object->orig_wndproc);

        FIXME("Created composition target %p for hwnd %p (subclassed, orig_wndproc %p).\n",
                object, hwnd, object->orig_wndproc);
    }

    *target = &object->IDCompositionTarget_iface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateVisual(IDCompositionDevice *iface,
        IDCompositionVisual **visual)
{
    struct dcomp_visual *object;

    TRACE("iface %p, visual %p.\n", iface, visual);

    if (!visual)
        return E_INVALIDARG;

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->IDCompositionVisual_iface.lpVtbl = &dcomp_visual_vtbl;
    object->refcount = 1;

    TRACE("Created composition visual %p.\n", object);

    *visual = &object->IDCompositionVisual_iface;
    return S_OK;
}

/* Typedef for D3D11CreateDevice loaded via GetProcAddress (no hard d3d11 import). */
typedef HRESULT (WINAPI *pD3D11CreateDevice)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE,
        UINT, const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **,
        D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

static HRESULT dcomp_device_create_standalone_d3d11(ID3D11Device **out)
{
    pD3D11CreateDevice create_fn;
    HMODULE d3d11_mod;
    HRESULT hr;

    d3d11_mod = LoadLibraryW(L"d3d11.dll");
    if (!d3d11_mod)
    {
        FIXME("Failed to load d3d11.dll for standalone D3D11 device.\n");
        return E_FAIL;
    }

    create_fn = (pD3D11CreateDevice)GetProcAddress(d3d11_mod, "D3D11CreateDevice");
    if (!create_fn)
    {
        FIXME("Failed to find D3D11CreateDevice in d3d11.dll.\n");
        FreeLibrary(d3d11_mod);
        return E_FAIL;
    }

    hr = create_fn(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
            D3D11_SDK_VERSION, out, NULL, NULL);
    if (FAILED(hr))
    {
        FIXME("D3D11CreateDevice(HARDWARE) failed: %#lx, trying WARP.\n", hr);
        hr = create_fn(NULL, D3D_DRIVER_TYPE_WARP, NULL,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                D3D11_SDK_VERSION, out, NULL, NULL);
    }
    if (FAILED(hr))
        FIXME("D3D11CreateDevice failed: %#lx.\n", hr);
    else
        FIXME("Created standalone D3D11 device %p for DComp surface rendering.\n", *out);

    /* Keep d3d11.dll loaded — it's already in-process anyway. */
    return hr;
}

static HRESULT dcomp_device_get_d3d11(struct dcomp_device *device, ID3D11Device **out)
{
    IDXGIDevice *dxgi_device;
    HRESULT hr;

    if (device->d3d11_device)
    {
        *out = device->d3d11_device;
        ID3D11Device_AddRef(*out);
        return S_OK;
    }

    if (!device->rendering_device)
    {
        FIXME("No rendering device — creating standalone D3D11 device.\n");
        goto create_standalone;
    }

    /* Try 1: QI the rendering device for IDXGIDevice, then for ID3D11Device */
    hr = IUnknown_QueryInterface(device->rendering_device,
            &IID_IDXGIDevice, (void **)&dxgi_device);
    if (SUCCEEDED(hr))
    {
        hr = IDXGIDevice_QueryInterface(dxgi_device,
                &IID_ID3D11Device, (void **)&device->d3d11_device);
        IDXGIDevice_Release(dxgi_device);
        if (SUCCEEDED(hr))
            goto done;
    }

    /* Try 2: Maybe it IS the D3D11 device directly */
    hr = IUnknown_QueryInterface(device->rendering_device,
            &IID_ID3D11Device, (void **)&device->d3d11_device);
    if (SUCCEEDED(hr))
        goto done;

    /* Try 3: Rendering device is likely an ID2D1Device (VSTGUI pattern) —
     * we cannot extract the underlying D3D11 device through public APIs.
     * Create a standalone D3D11 device for our surface textures. */
    FIXME("Cannot QI rendering device for ID3D11Device (%#lx) — creating standalone.\n", hr);

create_standalone:
    hr = dcomp_device_create_standalone_d3d11(&device->d3d11_device);
    if (FAILED(hr))
        return hr;

done:
    *out = device->d3d11_device;
    ID3D11Device_AddRef(*out);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateSurface(IDCompositionDevice *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice(iface);
    struct dcomp_surface *object;
    HRESULT hr;

    FIXME("iface %p, %ux%u, format %#x, alpha %#x, surface %p.\n",
            iface, width, height, pixel_format, alpha_mode, surface);

    if (!surface)
        return E_INVALIDARG;

    *surface = NULL;

    if (device->d2d1_device)
    {
        hr = dcomp_surface_create(NULL, device->d2d1_device, width, height,
                pixel_format, alpha_mode, FALSE, &object);
    }
    else
    {
        ID3D11Device *d3d11_device;
        hr = dcomp_device_get_d3d11(device, &d3d11_device);
        if (FAILED(hr))
            return hr;
        hr = dcomp_surface_create(d3d11_device, NULL, width, height,
                pixel_format, alpha_mode, FALSE, &object);
        ID3D11Device_Release(d3d11_device);
    }
    if (FAILED(hr))
        return hr;

    object->device_iface = &device->IDCompositionDevice_iface;
    IDCompositionDevice_AddRef(object->device_iface);
    *surface = &object->IDCompositionSurface_iface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateVirtualSurface(IDCompositionDevice *iface,
        UINT width, UINT height, DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice(iface);
    struct dcomp_surface *object;
    HRESULT hr;

    FIXME("iface %p, %ux%u, format %#x, alpha %#x, surface %p.\n",
            iface, width, height, pixel_format, alpha_mode, surface);

    if (!surface)
        return E_INVALIDARG;

    *surface = NULL;

    if (device->d2d1_device)
    {
        hr = dcomp_surface_create(NULL, device->d2d1_device, width, height,
                pixel_format, alpha_mode, TRUE, &object);
    }
    else
    {
        ID3D11Device *d3d11_device;
        hr = dcomp_device_get_d3d11(device, &d3d11_device);
        if (FAILED(hr))
            return hr;
        hr = dcomp_surface_create(d3d11_device, NULL, width, height,
                pixel_format, alpha_mode, TRUE, &object);
        ID3D11Device_Release(d3d11_device);
    }
    if (FAILED(hr))
        return hr;

    object->device_iface = &device->IDCompositionDevice_iface;
    IDCompositionDevice_AddRef(object->device_iface);
    *surface = (IDCompositionVirtualSurface *)&object->IDCompositionSurface_iface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateSurfaceFromHandle(IDCompositionDevice *iface,
        HANDLE handle, IUnknown **surface)
{
    FIXME("iface %p, handle %p, surface %p stub!\n", iface, handle, surface);

    *surface = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateSurfaceFromHwnd(IDCompositionDevice *iface,
        HWND hwnd, IUnknown **surface)
{
    FIXME("iface %p, hwnd %p, surface %p stub!\n", iface, hwnd, surface);

    *surface = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateTranslateTransform(IDCompositionDevice *iface,
        IDCompositionTranslateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);

    *transform = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateScaleTransform(IDCompositionDevice *iface,
        IDCompositionScaleTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);

    *transform = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateRotateTransform(IDCompositionDevice *iface,
        IDCompositionRotateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);

    *transform = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateSkewTransform(IDCompositionDevice *iface,
        IDCompositionSkewTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);

    *transform = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateMatrixTransform(IDCompositionDevice *iface,
        IDCompositionMatrixTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);

    *transform = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateTransformGroup(IDCompositionDevice *iface,
        IDCompositionTransform **transforms, UINT elements,
        IDCompositionTransform **transform_group)
{
    FIXME("iface %p, transforms %p, elements %u, group %p stub!\n",
            iface, transforms, elements, transform_group);

    *transform_group = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateTranslateTransform3D(IDCompositionDevice *iface,
        IDCompositionTranslateTransform3D **transform_3d)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform_3d);

    *transform_3d = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateScaleTransform3D(IDCompositionDevice *iface,
        IDCompositionScaleTransform3D **transform_3d)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform_3d);

    *transform_3d = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateRotateTransform3D(IDCompositionDevice *iface,
        IDCompositionRotateTransform3D **transform_3d)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform_3d);

    *transform_3d = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateMatrixTransform3D(IDCompositionDevice *iface,
        IDCompositionMatrixTransform3D **transform_3d)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform_3d);

    *transform_3d = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateTransform3DGroup(IDCompositionDevice *iface,
        IDCompositionTransform3D **transforms_3d, UINT elements,
        IDCompositionTransform3D **transform_3d_group)
{
    FIXME("iface %p, transforms %p, elements %u, group %p stub!\n",
            iface, transforms_3d, elements, transform_3d_group);

    *transform_3d_group = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateEffectGroup(IDCompositionDevice *iface,
        IDCompositionEffectGroup **effect_group)
{
    TRACE("iface %p, effect_group %p.\n", iface, effect_group);

    return dcomp_effect_group_create(effect_group);
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateRectangleClip(IDCompositionDevice *iface,
        IDCompositionRectangleClip **clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);

    *clip = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateAnimation(IDCompositionDevice *iface,
        IDCompositionAnimation **animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);

    *animation = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CheckDeviceState(IDCompositionDevice *iface,
        BOOL *valid)
{
    TRACE("iface %p, valid %p.\n", iface, valid);

    if (valid)
        *valid = TRUE;
    return S_OK;
}

static const IDCompositionDeviceVtbl dcomp_device_vtbl =
{
    dcomp_device_QueryInterface,
    dcomp_device_AddRef,
    dcomp_device_Release,
    dcomp_device_Commit,
    dcomp_device_WaitForCommitCompletion,
    dcomp_device_GetFrameStatistics,
    dcomp_device_CreateTargetForHwnd,
    dcomp_device_CreateVisual,
    dcomp_device_CreateSurface,
    dcomp_device_CreateVirtualSurface,
    dcomp_device_CreateSurfaceFromHandle,
    dcomp_device_CreateSurfaceFromHwnd,
    dcomp_device_CreateTranslateTransform,
    dcomp_device_CreateScaleTransform,
    dcomp_device_CreateRotateTransform,
    dcomp_device_CreateSkewTransform,
    dcomp_device_CreateMatrixTransform,
    dcomp_device_CreateTransformGroup,
    dcomp_device_CreateTranslateTransform3D,
    dcomp_device_CreateScaleTransform3D,
    dcomp_device_CreateRotateTransform3D,
    dcomp_device_CreateMatrixTransform3D,
    dcomp_device_CreateTransform3DGroup,
    dcomp_device_CreateEffectGroup,
    dcomp_device_CreateRectangleClip,
    dcomp_device_CreateAnimation,
    dcomp_device_CheckDeviceState,
};

/* =====================================================================
 * IDCompositionDesktopDevice (inherits IDCompositionDevice2)
 *
 * All methods delegate to the shared dcomp_device implementation.
 * The first parameter is IDCompositionDesktopDevice* so we need
 * thin wrappers that recover the dcomp_device from the correct offset.
 * ===================================================================== */

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_QueryInterface(
        IDCompositionDesktopDevice *iface, REFIID iid, void **out)
{
    struct dcomp_device *device = impl_from_IDCompositionDesktopDevice(iface);
    return dcomp_device_QueryInterface(&device->IDCompositionDevice_iface, iid, out);
}

static ULONG STDMETHODCALLTYPE dcomp_desktop_device_AddRef(IDCompositionDesktopDevice *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDesktopDevice(iface);
    return dcomp_device_AddRef(&device->IDCompositionDevice_iface);
}

static ULONG STDMETHODCALLTYPE dcomp_desktop_device_Release(IDCompositionDesktopDevice *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDesktopDevice(iface);
    return dcomp_device_Release(&device->IDCompositionDevice_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_Commit(IDCompositionDesktopDevice *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDesktopDevice(iface);
    return dcomp_device_Commit(&device->IDCompositionDevice_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_WaitForCommitCompletion(
        IDCompositionDesktopDevice *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDesktopDevice(iface);
    return dcomp_device_WaitForCommitCompletion(&device->IDCompositionDevice_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_GetFrameStatistics(
        IDCompositionDesktopDevice *iface, DCOMPOSITION_FRAME_STATISTICS *statistics)
{
    struct dcomp_device *device = impl_from_IDCompositionDesktopDevice(iface);
    return dcomp_device_GetFrameStatistics(&device->IDCompositionDevice_iface, statistics);
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateVisual(
        IDCompositionDesktopDevice *iface, IDCompositionVisual2 **visual)
{
    struct dcomp_device *device = impl_from_IDCompositionDesktopDevice(iface);
    IDCompositionVisual *v1 = NULL;
    HRESULT hr;

    TRACE("iface %p, visual %p.\n", iface, visual);

    if (!visual)
        return E_INVALIDARG;

    if (FAILED(hr = dcomp_device_CreateVisual(&device->IDCompositionDevice_iface, &v1)))
        return hr;

    /* The v1 path creates the object with the v1 vtable; swap it for the real
     * IDCompositionVisual2 vtable so the Visual2-only methods dispatch
     * correctly instead of running past the 20-slot v1 vtable. */
    impl_from_IDCompositionVisual(v1)->IDCompositionVisual_iface.lpVtbl =
            (const IDCompositionVisualVtbl *)&dcomp_visual2_vtbl;
    *visual = (IDCompositionVisual2 *)v1;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateSurfaceFactory(
        IDCompositionDesktopDevice *iface, IUnknown *rendering_device,
        IDCompositionSurfaceFactory **surface_factory)
{
    struct dcomp_device *device = impl_from_IDCompositionDesktopDevice(iface);
    struct dcomp_surface_factory *object;

    FIXME("iface %p, rendering_device %p, surface_factory %p.\n",
            iface, rendering_device, surface_factory);

    if (!surface_factory)
        return E_INVALIDARG;

    *surface_factory = NULL;

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->IDCompositionSurfaceFactory_iface.lpVtbl = &dcomp_surface_factory_vtbl;
    object->refcount = 1;
    object->device_iface = &device->IDCompositionDevice_iface;
    IDCompositionDevice_AddRef(object->device_iface);

    FIXME("Created surface factory %p for device %p.\n", object, device);

    *surface_factory = &object->IDCompositionSurfaceFactory_iface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateSurface(
        IDCompositionDesktopDevice *iface, UINT width, UINT height,
        DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    struct dcomp_device *device = impl_from_IDCompositionDesktopDevice(iface);
    return dcomp_device_CreateSurface(&device->IDCompositionDevice_iface,
            width, height, pixel_format, alpha_mode, surface);
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateVirtualSurface(
        IDCompositionDesktopDevice *iface, UINT width, UINT height,
        DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    struct dcomp_device *device = impl_from_IDCompositionDesktopDevice(iface);
    return dcomp_device_CreateVirtualSurface(&device->IDCompositionDevice_iface,
            width, height, pixel_format, alpha_mode, surface);
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateTranslateTransform(
        IDCompositionDesktopDevice *iface, IDCompositionTranslateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);

    *transform = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateScaleTransform(
        IDCompositionDesktopDevice *iface, IDCompositionScaleTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);

    *transform = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateRotateTransform(
        IDCompositionDesktopDevice *iface, IDCompositionRotateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);

    *transform = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateSkewTransform(
        IDCompositionDesktopDevice *iface, IDCompositionSkewTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);

    *transform = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateMatrixTransform(
        IDCompositionDesktopDevice *iface, IDCompositionMatrixTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);

    *transform = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateTransformGroup(
        IDCompositionDesktopDevice *iface, IDCompositionTransform **transforms,
        UINT elements, IDCompositionTransform **transform_group)
{
    FIXME("iface %p, transforms %p, elements %u, group %p stub!\n",
            iface, transforms, elements, transform_group);

    *transform_group = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateTranslateTransform3D(
        IDCompositionDesktopDevice *iface, IDCompositionTranslateTransform3D **transform_3d)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform_3d);

    *transform_3d = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateScaleTransform3D(
        IDCompositionDesktopDevice *iface, IDCompositionScaleTransform3D **transform_3d)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform_3d);

    *transform_3d = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateRotateTransform3D(
        IDCompositionDesktopDevice *iface, IDCompositionRotateTransform3D **transform_3d)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform_3d);

    *transform_3d = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateMatrixTransform3D(
        IDCompositionDesktopDevice *iface, IDCompositionMatrixTransform3D **transform_3d)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform_3d);

    *transform_3d = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateTransform3DGroup(
        IDCompositionDesktopDevice *iface, IDCompositionTransform3D **transforms_3d,
        UINT elements, IDCompositionTransform3D **transform_3d_group)
{
    FIXME("iface %p, transforms %p, elements %u, group %p stub!\n",
            iface, transforms_3d, elements, transform_3d_group);

    *transform_3d_group = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateEffectGroup(
        IDCompositionDesktopDevice *iface, IDCompositionEffectGroup **effect_group)
{
    TRACE("iface %p, effect_group %p.\n", iface, effect_group);

    return dcomp_effect_group_create(effect_group);
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateRectangleClip(
        IDCompositionDesktopDevice *iface, IDCompositionRectangleClip **clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);

    *clip = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateAnimation(
        IDCompositionDesktopDevice *iface, IDCompositionAnimation **animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);

    *animation = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateTargetForHwnd(
        IDCompositionDesktopDevice *iface, HWND hwnd, BOOL topmost,
        IDCompositionTarget **target)
{
    struct dcomp_device *device = impl_from_IDCompositionDesktopDevice(iface);
    return dcomp_device_CreateTargetForHwnd(&device->IDCompositionDevice_iface,
            hwnd, topmost, target);
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateSurfaceFromHandle(
        IDCompositionDesktopDevice *iface, HANDLE handle, IUnknown **surface)
{
    FIXME("iface %p, handle %p, surface %p stub!\n", iface, handle, surface);
    if (surface)
        *surface = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateSurfaceFromHwnd(
        IDCompositionDesktopDevice *iface, HWND hwnd, IUnknown **surface)
{
    FIXME("iface %p, hwnd %p, surface %p stub!\n", iface, hwnd, surface);
    if (surface)
        *surface = NULL;
    return E_NOTIMPL;
}

static const IDCompositionDesktopDeviceVtbl dcomp_desktop_device_vtbl =
{
    /* IUnknown */
    dcomp_desktop_device_QueryInterface,
    dcomp_desktop_device_AddRef,
    dcomp_desktop_device_Release,
    /* IDCompositionDevice2 */
    dcomp_desktop_device_Commit,
    dcomp_desktop_device_WaitForCommitCompletion,
    dcomp_desktop_device_GetFrameStatistics,
    dcomp_desktop_device_CreateVisual,
    dcomp_desktop_device_CreateSurfaceFactory,
    dcomp_desktop_device_CreateSurface,
    dcomp_desktop_device_CreateVirtualSurface,
    dcomp_desktop_device_CreateTranslateTransform,
    dcomp_desktop_device_CreateScaleTransform,
    dcomp_desktop_device_CreateRotateTransform,
    dcomp_desktop_device_CreateSkewTransform,
    dcomp_desktop_device_CreateMatrixTransform,
    dcomp_desktop_device_CreateTransformGroup,
    dcomp_desktop_device_CreateTranslateTransform3D,
    dcomp_desktop_device_CreateScaleTransform3D,
    dcomp_desktop_device_CreateRotateTransform3D,
    dcomp_desktop_device_CreateMatrixTransform3D,
    dcomp_desktop_device_CreateTransform3DGroup,
    dcomp_desktop_device_CreateEffectGroup,
    dcomp_desktop_device_CreateRectangleClip,
    dcomp_desktop_device_CreateAnimation,
    /* IDCompositionDesktopDevice */
    dcomp_desktop_device_CreateTargetForHwnd,
    dcomp_desktop_device_CreateSurfaceFromHandle,
    dcomp_desktop_device_CreateSurfaceFromHwnd,
};

/* =====================================================================
 * IDCompositionDevice3 / IDCompositionDevice4 / IDCompositionDevice5
 *
 * Chromium/WebView2 gates its entire DirectComposition presentation path
 * (ui/gl/direct_composition_support.cc: g_dcomp_device, DCLayerTree,
 * SwapChainPresenter) on a successful QI for IDCompositionDevice3 — the
 * effect factories themselves are never called there. The Device2-method
 * wrappers delegate to the desktop device; the 13 effect factories are
 * FIXME stubs until a caller actually needs them.
 *
 * Runtime 150's delegated-compositing path additionally QIs for
 * IDCompositionDevice5 (dc_layer_tree.cc) and uses the inherited Device4
 * methods CheckCompositionTextureSupport/CreateCompositionTexture
 * (issue 90). One embedded IDCompositionDevice5 iface answers the
 * Device3/4/5 QIs; the vtbl below carries all inherited slots.
 * ===================================================================== */

static HRESULT STDMETHODCALLTYPE dcomp_device3_QueryInterface(
        IDCompositionDevice5 *iface, REFIID iid, void **out)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_device_QueryInterface(&device->IDCompositionDevice_iface, iid, out);
}

static ULONG STDMETHODCALLTYPE dcomp_device3_AddRef(IDCompositionDevice5 *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_device_AddRef(&device->IDCompositionDevice_iface);
}

static ULONG STDMETHODCALLTYPE dcomp_device3_Release(IDCompositionDevice5 *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_device_Release(&device->IDCompositionDevice_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_Commit(IDCompositionDevice5 *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_Commit(&device->IDCompositionDesktopDevice_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_WaitForCommitCompletion(
        IDCompositionDevice5 *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_WaitForCommitCompletion(&device->IDCompositionDesktopDevice_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_GetFrameStatistics(
        IDCompositionDevice5 *iface, DCOMPOSITION_FRAME_STATISTICS *statistics)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_GetFrameStatistics(&device->IDCompositionDesktopDevice_iface,
            statistics);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateVisual(
        IDCompositionDevice5 *iface, IDCompositionVisual2 **visual)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_CreateVisual(&device->IDCompositionDesktopDevice_iface, visual);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateSurfaceFactory(
        IDCompositionDevice5 *iface, IUnknown *rendering_device,
        IDCompositionSurfaceFactory **surface_factory)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_CreateSurfaceFactory(&device->IDCompositionDesktopDevice_iface,
            rendering_device, surface_factory);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateSurface(
        IDCompositionDevice5 *iface, UINT width, UINT height,
        DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_CreateSurface(&device->IDCompositionDesktopDevice_iface,
            width, height, pixel_format, alpha_mode, surface);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateVirtualSurface(
        IDCompositionDevice5 *iface, UINT width, UINT height,
        DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_CreateVirtualSurface(&device->IDCompositionDesktopDevice_iface,
            width, height, pixel_format, alpha_mode, surface);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateTranslateTransform(
        IDCompositionDevice5 *iface, IDCompositionTranslateTransform **transform)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_CreateTranslateTransform(&device->IDCompositionDesktopDevice_iface,
            transform);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateScaleTransform(
        IDCompositionDevice5 *iface, IDCompositionScaleTransform **transform)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_CreateScaleTransform(&device->IDCompositionDesktopDevice_iface,
            transform);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateRotateTransform(
        IDCompositionDevice5 *iface, IDCompositionRotateTransform **transform)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_CreateRotateTransform(&device->IDCompositionDesktopDevice_iface,
            transform);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateSkewTransform(
        IDCompositionDevice5 *iface, IDCompositionSkewTransform **transform)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_CreateSkewTransform(&device->IDCompositionDesktopDevice_iface,
            transform);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateMatrixTransform(
        IDCompositionDevice5 *iface, IDCompositionMatrixTransform **transform)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_CreateMatrixTransform(&device->IDCompositionDesktopDevice_iface,
            transform);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateTransformGroup(
        IDCompositionDevice5 *iface, IDCompositionTransform **transforms,
        UINT elements, IDCompositionTransform **transform_group)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_CreateTransformGroup(&device->IDCompositionDesktopDevice_iface,
            transforms, elements, transform_group);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateTranslateTransform3D(
        IDCompositionDevice5 *iface, IDCompositionTranslateTransform3D **transform_3d)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_CreateTranslateTransform3D(&device->IDCompositionDesktopDevice_iface,
            transform_3d);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateScaleTransform3D(
        IDCompositionDevice5 *iface, IDCompositionScaleTransform3D **transform_3d)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_CreateScaleTransform3D(&device->IDCompositionDesktopDevice_iface,
            transform_3d);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateRotateTransform3D(
        IDCompositionDevice5 *iface, IDCompositionRotateTransform3D **transform_3d)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_CreateRotateTransform3D(&device->IDCompositionDesktopDevice_iface,
            transform_3d);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateMatrixTransform3D(
        IDCompositionDevice5 *iface, IDCompositionMatrixTransform3D **transform_3d)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_CreateMatrixTransform3D(&device->IDCompositionDesktopDevice_iface,
            transform_3d);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateTransform3DGroup(
        IDCompositionDevice5 *iface, IDCompositionTransform3D **transforms_3d,
        UINT elements, IDCompositionTransform3D **transform_3d_group)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_CreateTransform3DGroup(&device->IDCompositionDesktopDevice_iface,
            transforms_3d, elements, transform_3d_group);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateEffectGroup(
        IDCompositionDevice5 *iface, IDCompositionEffectGroup **effect_group)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_CreateEffectGroup(&device->IDCompositionDesktopDevice_iface,
            effect_group);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateRectangleClip(
        IDCompositionDevice5 *iface, IDCompositionRectangleClip **clip)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_CreateRectangleClip(&device->IDCompositionDesktopDevice_iface, clip);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateAnimation(
        IDCompositionDevice5 *iface, IDCompositionAnimation **animation)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice5(iface);
    return dcomp_desktop_device_CreateAnimation(&device->IDCompositionDesktopDevice_iface, animation);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateGaussianBlurEffect(
        IDCompositionDevice5 *iface, IDCompositionGaussianBlurEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateBrightnessEffect(
        IDCompositionDevice5 *iface, IDCompositionBrightnessEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateColorMatrixEffect(
        IDCompositionDevice5 *iface, IDCompositionColorMatrixEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateShadowEffect(
        IDCompositionDevice5 *iface, IDCompositionShadowEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateHueRotationEffect(
        IDCompositionDevice5 *iface, IDCompositionHueRotationEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateSaturationEffect(
        IDCompositionDevice5 *iface, IDCompositionSaturationEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateTurbulenceEffect(
        IDCompositionDevice5 *iface, IDCompositionTurbulenceEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateLinearTransferEffect(
        IDCompositionDevice5 *iface, IDCompositionLinearTransferEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateTableTransferEffect(
        IDCompositionDevice5 *iface, IDCompositionTableTransferEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateCompositeEffect(
        IDCompositionDevice5 *iface, IDCompositionCompositeEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateBlendEffect(
        IDCompositionDevice5 *iface, IDCompositionBlendEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateArithmeticCompositeEffect(
        IDCompositionDevice5 *iface, IDCompositionArithmeticCompositeEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateAffineTransform2DEffect(
        IDCompositionDevice5 *iface, IDCompositionAffineTransform2DEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device4_CheckCompositionTextureSupport(
        IDCompositionDevice5 *iface, IUnknown *rendering_device, BOOL *supported)
{
    ID3D11Device *d3d11_device;

    TRACE("iface %p, rendering_device %p, supported %p.\n", iface, rendering_device, supported);

    if (!supported)
        return E_POINTER;
    *supported = FALSE;
    if (!rendering_device)
        return E_INVALIDARG;

    /* Composition textures are supported for D3D11 rendering devices — the
     * readback compositor only needs CopyResource + Map on the texture's
     * own device/context. */
    if (SUCCEEDED(IUnknown_QueryInterface(rendering_device, &IID_ID3D11Device,
            (void **)&d3d11_device)))
    {
        ID3D11Device_Release(d3d11_device);
        *supported = TRUE;
    }

    FIXME("rendering_device %p -> supported %d.\n", rendering_device, *supported);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_device4_CreateCompositionTexture(
        IDCompositionDevice5 *iface, IUnknown *d3d_texture,
        IDCompositionTexture **composition_texture)
{
    struct dcomp_texture *object;
    ID3D11Texture2D *texture;
    HRESULT hr;

    TRACE("iface %p, d3d_texture %p, composition_texture %p.\n",
            iface, d3d_texture, composition_texture);

    if (!composition_texture)
        return E_POINTER;
    *composition_texture = NULL;
    if (!d3d_texture)
        return E_INVALIDARG;

    if (FAILED(hr = IUnknown_QueryInterface(d3d_texture, &IID_ID3D11Texture2D,
            (void **)&texture)))
    {
        FIXME("d3d_texture %p is not an ID3D11Texture2D, hr %#lx.\n", d3d_texture, hr);
        return E_INVALIDARG;
    }

    if (!(object = calloc(1, sizeof(*object))))
    {
        ID3D11Texture2D_Release(texture);
        return E_OUTOFMEMORY;
    }

    object->IDCompositionTexture_iface.lpVtbl = &dcomp_texture_vtbl;
    object->refcount = 1;
    object->texture = texture;  /* keeps the QI reference */
    ID3D11Texture2D_GetDesc(texture, &object->desc);
    object->alpha_mode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    object->color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;

    *composition_texture = &object->IDCompositionTexture_iface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dcomp_device5_CreateDynamicTexture(
        IDCompositionDevice5 *iface, IDCompositionDynamicTexture **texture)
{
    struct dcomp_dynamic_texture *object;

    TRACE("iface %p, texture %p.\n", iface, texture);

    if (!texture)
        return E_POINTER;
    *texture = NULL;

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->IDCompositionDynamicTexture_iface.lpVtbl = &dcomp_dynamic_texture_vtbl;
    object->refcount = 1;

    *texture = &object->IDCompositionDynamicTexture_iface;
    return S_OK;
}

static const IDCompositionDevice5Vtbl dcomp_device5_vtbl =
{
    /* IUnknown */
    dcomp_device3_QueryInterface,
    dcomp_device3_AddRef,
    dcomp_device3_Release,
    /* IDCompositionDevice2 */
    dcomp_device3_Commit,
    dcomp_device3_WaitForCommitCompletion,
    dcomp_device3_GetFrameStatistics,
    dcomp_device3_CreateVisual,
    dcomp_device3_CreateSurfaceFactory,
    dcomp_device3_CreateSurface,
    dcomp_device3_CreateVirtualSurface,
    dcomp_device3_CreateTranslateTransform,
    dcomp_device3_CreateScaleTransform,
    dcomp_device3_CreateRotateTransform,
    dcomp_device3_CreateSkewTransform,
    dcomp_device3_CreateMatrixTransform,
    dcomp_device3_CreateTransformGroup,
    dcomp_device3_CreateTranslateTransform3D,
    dcomp_device3_CreateScaleTransform3D,
    dcomp_device3_CreateRotateTransform3D,
    dcomp_device3_CreateMatrixTransform3D,
    dcomp_device3_CreateTransform3DGroup,
    dcomp_device3_CreateEffectGroup,
    dcomp_device3_CreateRectangleClip,
    dcomp_device3_CreateAnimation,
    /* IDCompositionDevice3 */
    dcomp_device3_CreateGaussianBlurEffect,
    dcomp_device3_CreateBrightnessEffect,
    dcomp_device3_CreateColorMatrixEffect,
    dcomp_device3_CreateShadowEffect,
    dcomp_device3_CreateHueRotationEffect,
    dcomp_device3_CreateSaturationEffect,
    dcomp_device3_CreateTurbulenceEffect,
    dcomp_device3_CreateLinearTransferEffect,
    dcomp_device3_CreateTableTransferEffect,
    dcomp_device3_CreateCompositeEffect,
    dcomp_device3_CreateBlendEffect,
    dcomp_device3_CreateArithmeticCompositeEffect,
    dcomp_device3_CreateAffineTransform2DEffect,
    /* IDCompositionDevice4 */
    dcomp_device4_CheckCompositionTextureSupport,
    dcomp_device4_CreateCompositionTexture,
    /* IDCompositionDevice5 */
    dcomp_device5_CreateDynamicTexture,
};

/* =====================================================================
 * Entry points
 * ===================================================================== */

static HRESULT dcomp_device_create(IUnknown *rendering_device, REFIID iid, void **device)
{
    struct dcomp_device *object;
    HRESULT hr;

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->IDCompositionDevice_iface.lpVtbl = &dcomp_device_vtbl;
    object->IDCompositionDesktopDevice_iface.lpVtbl = &dcomp_desktop_device_vtbl;
    object->IDCompositionDevice5_iface.lpVtbl = &dcomp_device5_vtbl;
    object->refcount = 1;
    InitializeCriticalSection(&object->cs);

    if (rendering_device)
    {
        object->rendering_device = rendering_device;
        IUnknown_AddRef(rendering_device);

        /* Try to QI for ID2D1Device — VSTGUI passes an ID2D1Device as the
         * rendering_device. If successful, we use the D2D1 bitmap path so
         * that DeviceContexts and resources are on the same device chain. */
        hr = IUnknown_QueryInterface(rendering_device,
                &IID_ID2D1Device, (void **)&object->d2d1_device);
        if (SUCCEEDED(hr))
            FIXME("Rendering device is ID2D1Device %p — using D2D1 bitmap path.\n",
                    object->d2d1_device);
    }

    FIXME("Created dcomp device %p (rendering_device %p, d2d1_device %p).\n",
            object, rendering_device, object->d2d1_device);

    return IDCompositionDevice_QueryInterface(&object->IDCompositionDevice_iface, iid, device);
}

HRESULT WINAPI DCompositionCreateSurfaceHandle(DWORD desired_access,
        SECURITY_ATTRIBUTES *security_attributes, HANDLE *surface_handle)
{
    /* This export was previously a spec stub, so a GetProcAddress feature
     * check by Chromium failed silently.  Hand out a plain event handle so
     * caller-side NULL checks pass; the consumers (CreateSurfaceFromHandle,
     * CreateSwapChainForCompositionSurfaceHandle) carry their own FIXME
     * markers and still fail visibly. */
    if (!surface_handle)
        return E_INVALIDARG;
    *surface_handle = CreateEventW(security_attributes, FALSE, FALSE, NULL);
    return *surface_handle ? S_OK : E_FAIL;
}

HRESULT WINAPI DCompositionCreateDevice(IDXGIDevice *dxgi_device, REFIID iid, void **device)
{
    TRACE("dxgi_device %p, iid %s, device %p.\n", dxgi_device, debugstr_guid(iid), device);

    return dcomp_device_create((IUnknown *)dxgi_device, iid, device);
}

HRESULT WINAPI DCompositionCreateDevice2(IUnknown *rendering_device, REFIID iid, void **device)
{
    TRACE("rendering_device %p, iid %s, device %p.\n", rendering_device, debugstr_guid(iid), device);

    return dcomp_device_create(rendering_device, iid, device);
}

HRESULT WINAPI DCompositionCreateDevice3(IUnknown *rendering_device, REFIID iid, void **device)
{
    TRACE("rendering_device %p, iid %s, device %p.\n", rendering_device, debugstr_guid(iid), device);

    return dcomp_device_create(rendering_device, iid, device);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        dcomp_heap = HeapCreate(0, 0, 0);
        if (!dcomp_heap) return FALSE;
        DisableThreadLibraryCalls(inst);
        break;
    case DLL_PROCESS_DETACH:
        if (reserved) break;
        if (dcomp_heap)
        {
            HeapDestroy(dcomp_heap);
            dcomp_heap = NULL;
        }
        break;
    }
    return TRUE;
}
