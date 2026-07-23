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
#include "d2d1_1.h"
#include "dcomp.h"
#include "wine/debug.h"

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

/* Forward declarations for target back-pointer and list management */
struct dcomp_device;
struct dcomp_target;
static void dcomp_device_remove_target(struct dcomp_device *device, struct dcomp_target *target);

/* Send child mode message to a swapchain's comp window */
/* Periodic tree compositing for rootless trees (Chromium/WebView2, issue 88):
 * the root visual carries no content, the swapchains hang on nested leaf
 * visuals, so no root Present ever composites them. The timer drives the
 * target-side composite in dcomp_target_composite_tree(). */
#define DCOMP_TREE_TIMER     ((UINT_PTR)0xDC0FFEE2)
#define DCOMP_TREE_TIMER_MS  100
#define DCOMP_TREE_FRAME_MS  16   /* ~60 Hz rate limit for hook-driven composites */

static void dcomp_send_child_mode(IUnknown *content)
{
    WCHAR prop_name[64];
    HWND comp_wnd;

    if (!content)
        return;

    swprintf(prop_name, ARRAY_SIZE(prop_name),
            L"__wine_dcomp_wnd_%I64x", (UINT_PTR)content);
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

/* Lazy-init the persistent D2D1 context and bitmaps for the D2D1Device path. */
static HRESULT dcomp_surface_ensure_d2d1_resources(struct dcomp_surface *surface)
{
    D2D1_BITMAP_PROPERTIES1 bmp_props;
    D2D1_SIZE_U size;
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

    size.width = surface->width;
    size.height = surface->height;

    /* Render-target bitmap (GPU-backed, TARGET flag) */
    memset(&bmp_props, 0, sizeof(bmp_props));
    bmp_props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bmp_props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bmp_props.dpiX = 96.0f;
    bmp_props.dpiY = 96.0f;
    bmp_props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;

    hr = ID2D1DeviceContext_CreateBitmap(surface->persistent_context,
            size, NULL, 0, &bmp_props, &surface->target_bitmap);
    if (FAILED(hr))
    {
        FIXME("CreateBitmap(TARGET) failed: %#lx.\n", hr);
        ID2D1DeviceContext_Release(surface->persistent_context);
        surface->persistent_context = NULL;
        return hr;
    }

    /* CPU-readable bitmap for readback (CANNOT_DRAW | CPU_READ) */
    memset(&bmp_props, 0, sizeof(bmp_props));
    bmp_props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bmp_props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bmp_props.dpiX = 96.0f;
    bmp_props.dpiY = 96.0f;
    bmp_props.bitmapOptions = D2D1_BITMAP_OPTIONS_CANNOT_DRAW | D2D1_BITMAP_OPTIONS_CPU_READ;

    hr = ID2D1DeviceContext_CreateBitmap(surface->persistent_context,
            size, NULL, 0, &bmp_props, &surface->readback_bitmap);
    if (FAILED(hr))
    {
        FIXME("CreateBitmap(CPU_READ) failed: %#lx.\n", hr);
        ID2D1Bitmap1_Release(surface->target_bitmap);
        surface->target_bitmap = NULL;
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

    if (IsEqualGUID(iid, &IID_IDXGISurface))
    {
        if (!surface->dxgi_surface)
        {
            WARN("Surface has no DXGI surface.\n");
            return E_FAIL;
        }
        IDXGISurface_AddRef(surface->dxgi_surface);
        *object = surface->dxgi_surface;
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
    IDXGISurface *new_dxgi_surface = NULL;
    DWORD *new_bits;

    TRACE("iface %p, %ux%u (old %ux%u).\n", iface, width, height, surface->width, surface->height);

    if (width == surface->width && height == surface->height)
        return S_OK;

    if (surface->drawing)
    {
        WARN("Resize called during active BeginDraw — ignoring.\n");
        return DXGI_ERROR_INVALID_CALL;
    }

    /* Two-phase resize: create ALL new resources first; only if everything
     * succeeds do we release the old ones and swap in the new size.  On any
     * failure the surface is left fully intact (old bits/textures/dimensions),
     * so a failed Resize cannot leave new-size metadata with missing GPU
     * objects (which would make later draws fail unpredictably). */

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

    /* Phase 2 — everything created: release old resources and swap in the new. */
    free(surface->bits);
    surface->bits = new_bits;

    /* D2D1 bitmaps are recreated lazily in BeginDraw via
     * dcomp_surface_ensure_d2d1_resources() at the new dimensions. */
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
    if (surface->persistent_context)
    {
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

        hr = ID3D11Device_CreateTexture2D(d3d11_device, &tex_desc, NULL, &surface->texture);
        if (FAILED(hr))
        {
            FIXME("Failed to create render target texture: %#lx.\n", hr);
            goto fail;
        }

        tex_desc.Usage = D3D11_USAGE_STAGING;
        tex_desc.BindFlags = 0;
        tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

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

struct dcomp_visual
{
    IDCompositionVisual IDCompositionVisual_iface;
    LONG refcount;
    IUnknown *content;
    struct dcomp_surface *surface_content; /* non-NULL if content is a DComp surface */
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
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    return S_OK;
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
    if (content)
    {
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
    }

    /* Only redirect root visuals (no parent) to target_hwnd.
     * Child visuals keep their hidden comp window; the root's
     * Present composites them via Porter-Duff Over.
     * Surface content does not need swapchain reparenting. */
    if (visual->surface_content)
    {
        /* Surface content: no swapchain to reparent. The composition
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
            L"__wine_dcomp_wnd_%I64x", (UINT_PTR)visual->content);
    comp_wnd = (HWND)GetPropW(GetDesktopWindow(), prop_name);

    if (!comp_wnd)
    {
        FIXME("Composition window NOT FOUND for content %p (prop: __wine_dcomp_wnd_%I64x).\n",
                visual->content, (UINT_PTR)visual->content);
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
    LONGLONG last_present_qpc;            /* QPC of last actual present — drives ~60 Hz coalescing (issue 56) */
    BOOL foreign;                         /* target hwnd belongs to another process — no subclass, hook-driven compositing (issue 88) */
    DWORD last_tree_composite_tick;       /* GetTickCount of last hook-driven tree composite (~60 Hz rate limit) */
    /* WndProc subclass for WM_ERASEBKGND / WM_PAINT protection (Phase 5) */
    WNDPROC orig_wndproc;
};

static inline struct dcomp_target *impl_from_IDCompositionTarget(IDCompositionTarget *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_target, IDCompositionTarget_iface);
}

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
        /* Remove from device's target list */
        if (target->device)
            dcomp_device_remove_target(target->device, target);

        /* Stop tree compositing (safe even if the hwnd is already gone) */
        if (target->hwnd)
            KillTimer(target->hwnd, DCOMP_TREE_TIMER);

        /* Remove WndProc subclass (Phase 5) */
        if (target->orig_wndproc && target->hwnd && IsWindow(target->hwnd))
        {
            SetWindowLongPtrW(target->hwnd, GWLP_WNDPROC, (LONG_PTR)target->orig_wndproc);
            RemovePropW(target->hwnd, dcomp_target_prop);
            FIXME("Removed WndProc subclass from hwnd %p.\n", target->hwnd);
        }

        /* Clean up presentation state */
        if (target->comp_bitmap)
        {
            SelectObject(target->comp_dc, NULL);
            DeleteObject(target->comp_bitmap);
        }
        if (target->comp_dc)
            DeleteDC(target->comp_dc);

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
    IDCompositionDevice3 IDCompositionDevice3_iface;
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

static inline struct dcomp_device *impl_from_IDCompositionDevice3(IDCompositionDevice3 *iface)
{
    return CONTAINING_RECORD(iface, struct dcomp_device, IDCompositionDevice3_iface);
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

    if (IsEqualGUID(iid, &IID_IDCompositionDevice3))
    {
        *out = &device->IDCompositionDevice3_iface;
        IDCompositionDevice3_AddRef(*out);
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

static void dcomp_target_composite_tree(struct dcomp_target *target);

/* Depth-first leaf serialization with accumulated offsets. The visual's own
 * offset positions its whole subtree (DComp semantics), so it is added on
 * entry. A content-bearing visual is serialized before its children (children
 * render on top). Container visuals (no content) only pass offsets down. */
static unsigned int dcomp_serialize_visual_leaves(HWND target_hwnd, struct dcomp_visual *visual,
        int base_x, int base_y, unsigned int idx)
{
    WCHAR prop_name[64];
    struct dcomp_visual *child;
    int vx = base_x + (int)visual->offset_x;
    int vy = base_y + (int)visual->offset_y;

    if (idx >= 16)
        return idx;

    /* Surface content: store bits pointer and size directly.
     * No window indirection needed — wined3d reads bits directly. */
    if (visual->surface_content)
    {
        struct dcomp_surface *surf = visual->surface_content;

        swprintf(prop_name, ARRAY_SIZE(prop_name),
                L"__wine_dcomp_child_%u_wnd", idx);
        SetPropW(target_hwnd, prop_name, (HANDLE)0);

        swprintf(prop_name, ARRAY_SIZE(prop_name),
                L"__wine_dcomp_child_%u_bits", idx);
        SetPropW(target_hwnd, prop_name, (HANDLE)surf->bits);

        swprintf(prop_name, ARRAY_SIZE(prop_name),
                L"__wine_dcomp_child_%u_size", idx);
        SetPropW(target_hwnd, prop_name, (HANDLE)(ULONG_PTR)MAKELPARAM(surf->width, surf->height));

        /* Pack offset as two signed 16-bit values */
        swprintf(prop_name, ARRAY_SIZE(prop_name),
                L"__wine_dcomp_child_%u_offset", idx);
        SetPropW(target_hwnd, prop_name, (HANDLE)(ULONG_PTR)MAKELPARAM(vx, vy));

        ++idx;
    }
    else if (visual->content)
    {
        /* Swapchain content: look up the visual's composition window */
        HWND child_comp_wnd;

        swprintf(prop_name, ARRAY_SIZE(prop_name),
                L"__wine_dcomp_wnd_%I64x", (UINT_PTR)visual->content);
        child_comp_wnd = (HWND)GetPropW(GetDesktopWindow(), prop_name);
        if (child_comp_wnd)
        {
            swprintf(prop_name, ARRAY_SIZE(prop_name),
                    L"__wine_dcomp_child_%u_wnd", idx);
            SetPropW(target_hwnd, prop_name, (HANDLE)child_comp_wnd);

            /* Pack offset as two signed 16-bit values */
            swprintf(prop_name, ARRAY_SIZE(prop_name),
                    L"__wine_dcomp_child_%u_offset", idx);
            SetPropW(target_hwnd, prop_name, (HANDLE)(ULONG_PTR)MAKELPARAM(vx, vy));

            ++idx;
        }
    }

    for (child = visual->children; child && idx < 16; child = child->next_sibling)
        idx = dcomp_serialize_visual_leaves(target_hwnd, child, vx, vy, idx);

    return idx;
}

static void dcomp_commit_visual_tree(HWND target_hwnd, struct dcomp_visual *root)
{
    unsigned int idx = 0;

    if (!root || !root->children || !target_hwnd)
    {
        if (target_hwnd)
        {
            SetPropW(target_hwnd, L"__wine_dcomp_child_count", (HANDLE)0);
            KillTimer(target_hwnd, DCOMP_TREE_TIMER);
        }
        return;
    }

    /* The root's own content is presented by its swapchain/surface path and
     * must NOT be serialized as a child (it would composite onto itself) —
     * start the walk at its children. */
    {
        struct dcomp_visual *child;
        for (child = root->children; child && idx < 16; child = child->next_sibling)
            idx = dcomp_serialize_visual_leaves(target_hwnd, child,
                    (int)root->offset_x, (int)root->offset_y, idx);
    }

    SetPropW(target_hwnd, L"__wine_dcomp_child_count", (HANDLE)(ULONG_PTR)idx);

    /* Rootless tree (Chromium): no root Present will ever composite the
     * leaves. In-process targets get the 100ms timer as backstop; foreign-
     * process targets cannot be subclassed, so their only drive is this
     * hook (Chromium calls SetContent per frame). The hook-driven composite
     * also runs for in-process targets — rate-limited to ~60 Hz — so page
     * updates don't wait for the timer. With a content-bearing root the
     * swapchain Present path handles compositing. */
    if (idx > 0 && !root->content && !root->surface_content)
    {
        struct dcomp_target *target = (struct dcomp_target *)GetPropW(target_hwnd, dcomp_target_prop);
        DWORD now = GetTickCount();

        if (target && !target->foreign)
            SetTimer(target_hwnd, DCOMP_TREE_TIMER, DCOMP_TREE_TIMER_MS, NULL);
        if (target && now - target->last_tree_composite_tick >= DCOMP_TREE_FRAME_MS)
        {
            target->last_tree_composite_tick = now;
            dcomp_target_composite_tree(target);
        }
    }
    else
        KillTimer(target_hwnd, DCOMP_TREE_TIMER);

    {
        static unsigned int commit_count;
        if (++commit_count <= 5 || !(commit_count % 100))
            FIXME("Commit #%u: serialized %u child visuals for target %p.\n",
                    commit_count, idx, target_hwnd);
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

    FIXME("Created comp DC %p with %ux%u DIB for target hwnd %p.\n",
            target->comp_dc, width, height, target->hwnd);
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

static void dcomp_target_composite_leaves(struct dcomp_target *target, struct dcomp_visual *visual,
        int base_x, int base_y)
{
    struct dcomp_visual *child;
    int vx = base_x + (int)visual->offset_x;
    int vy = base_y + (int)visual->offset_y;

    if (visual->surface_content && visual->surface_content->bits)
    {
        dcomp_composite_premul_over(target->comp_bits, target->comp_width, target->comp_height,
                visual->surface_content->bits, visual->surface_content->width,
                visual->surface_content->height, vx, vy);
    }
    else if (visual->content)
    {
        WCHAR prop_name[64];
        HWND comp_wnd;
        DWORD *bits;
        ULONG_PTR dims;

        /* Same lookup the serializer uses: comp window via desktop prop. */
        swprintf(prop_name, ARRAY_SIZE(prop_name),
                L"__wine_dcomp_wnd_%I64x", (UINT_PTR)visual->content);
        comp_wnd = (HWND)GetPropW(GetDesktopWindow(), prop_name);
        if (comp_wnd)
        {
            bits = (DWORD *)GetPropW(comp_wnd, L"__wine_dcomp_comp_bits");
            dims = (ULONG_PTR)GetPropW(comp_wnd, L"__wine_dcomp_comp_size");
            if (bits && dims)
                dcomp_composite_premul_over(target->comp_bits, target->comp_width,
                        target->comp_height, bits, LOWORD(dims), HIWORD(dims), vx, vy);
        }
    }

    for (child = visual->children; child; child = child->next_sibling)
        dcomp_target_composite_leaves(target, child, vx, vy);
}

static void dcomp_target_composite_tree(struct dcomp_target *target)
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

    GetClientRect(target->hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0)
        return;

    EnterCriticalSection(&target->device->cs);
    dcomp_target_ensure_comp_dc(target, rc.right, rc.bottom);
    if (target->comp_bits)
    {
        static unsigned int comp_tree_log;
        HDC hdc_win = GetDC(target->hwnd);

        /* Capture the live window content as backdrop: transparent page
         * areas must show what is behind (loader artwork / host content),
         * not opaque black (issue 88). Falls back to black when the capture
         * fails (e.g. unreadable foreign window DC). */
        if (!hdc_win || !BitBlt(target->comp_dc, 0, 0, rc.right, rc.bottom,
                hdc_win, 0, 0, SRCCOPY))
            memset(target->comp_bits, 0, (SIZE_T)rc.right * rc.bottom * sizeof(DWORD));
        if (hdc_win)
            ReleaseDC(target->hwnd, hdc_win);

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
        hdc = GetDC(target->hwnd);
        if (hdc)
        {
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, target->comp_dc, 0, 0, SRCCOPY);
            ReleaseDC(target->hwnd, hdc);
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
        return DefWindowProcW(hwnd, msg, wparam, lparam);

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
            dcomp_target_composite_tree(target);
            return 0;
        }
        break;

    case WM_PAINT:
        /* Rootless tree (issue 88): repaint from the composed tree instead of
         * the original wndproc (which would show the empty/loader content).
         * Content-bearing roots keep the VSTGUI paint path. */
        if (target->root_visual && !target->root_visual->content
                && !target->root_visual->surface_content)
        {
            PAINTSTRUCT ps;

            dcomp_target_composite_tree(target);
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;

    case WM_NCDESTROY:
        /* Window is being destroyed — clean up subclass and forward */
        KillTimer(hwnd, DCOMP_TREE_TIMER);
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)orig_wndproc);
        RemovePropW(hwnd, dcomp_target_prop);
        target->orig_wndproc = NULL;
        return CallWindowProcW(orig_wndproc, hwnd, msg, wparam, lparam);
    }

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
    struct dcomp_target *object;

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

    object->orig_wndproc = (WNDPROC)SetWindowLongPtrW(hwnd,
            GWLP_WNDPROC, (LONG_PTR)dcomp_target_wndproc);

    FIXME("Created composition target %p for hwnd %p (subclassed, orig_wndproc %p).\n",
            object, hwnd, object->orig_wndproc);

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
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateSurfaceFromHwnd(IDCompositionDevice *iface,
        HWND hwnd, IUnknown **surface)
{
    FIXME("iface %p, hwnd %p, surface %p stub!\n", iface, hwnd, surface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateTranslateTransform(IDCompositionDevice *iface,
        IDCompositionTranslateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateScaleTransform(IDCompositionDevice *iface,
        IDCompositionScaleTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateRotateTransform(IDCompositionDevice *iface,
        IDCompositionRotateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateSkewTransform(IDCompositionDevice *iface,
        IDCompositionSkewTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateMatrixTransform(IDCompositionDevice *iface,
        IDCompositionMatrixTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateTransformGroup(IDCompositionDevice *iface,
        IDCompositionTransform **transforms, UINT elements,
        IDCompositionTransform **transform_group)
{
    FIXME("iface %p, transforms %p, elements %u, group %p stub!\n",
            iface, transforms, elements, transform_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateTranslateTransform3D(IDCompositionDevice *iface,
        IDCompositionTranslateTransform3D **transform_3d)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateScaleTransform3D(IDCompositionDevice *iface,
        IDCompositionScaleTransform3D **transform_3d)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateRotateTransform3D(IDCompositionDevice *iface,
        IDCompositionRotateTransform3D **transform_3d)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateMatrixTransform3D(IDCompositionDevice *iface,
        IDCompositionMatrixTransform3D **transform_3d)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateTransform3DGroup(IDCompositionDevice *iface,
        IDCompositionTransform3D **transforms_3d, UINT elements,
        IDCompositionTransform3D **transform_3d_group)
{
    FIXME("iface %p, transforms %p, elements %u, group %p stub!\n",
            iface, transforms_3d, elements, transform_3d_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateEffectGroup(IDCompositionDevice *iface,
        IDCompositionEffectGroup **effect_group)
{
    FIXME("iface %p, effect_group %p stub!\n", iface, effect_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateRectangleClip(IDCompositionDevice *iface,
        IDCompositionRectangleClip **clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device_CreateAnimation(IDCompositionDevice *iface,
        IDCompositionAnimation **animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
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
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateScaleTransform(
        IDCompositionDesktopDevice *iface, IDCompositionScaleTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateRotateTransform(
        IDCompositionDesktopDevice *iface, IDCompositionRotateTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateSkewTransform(
        IDCompositionDesktopDevice *iface, IDCompositionSkewTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateMatrixTransform(
        IDCompositionDesktopDevice *iface, IDCompositionMatrixTransform **transform)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateTransformGroup(
        IDCompositionDesktopDevice *iface, IDCompositionTransform **transforms,
        UINT elements, IDCompositionTransform **transform_group)
{
    FIXME("iface %p, transforms %p, elements %u, group %p stub!\n",
            iface, transforms, elements, transform_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateTranslateTransform3D(
        IDCompositionDesktopDevice *iface, IDCompositionTranslateTransform3D **transform_3d)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateScaleTransform3D(
        IDCompositionDesktopDevice *iface, IDCompositionScaleTransform3D **transform_3d)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateRotateTransform3D(
        IDCompositionDesktopDevice *iface, IDCompositionRotateTransform3D **transform_3d)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateMatrixTransform3D(
        IDCompositionDesktopDevice *iface, IDCompositionMatrixTransform3D **transform_3d)
{
    FIXME("iface %p, transform %p stub!\n", iface, transform_3d);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateTransform3DGroup(
        IDCompositionDesktopDevice *iface, IDCompositionTransform3D **transforms_3d,
        UINT elements, IDCompositionTransform3D **transform_3d_group)
{
    FIXME("iface %p, transforms %p, elements %u, group %p stub!\n",
            iface, transforms_3d, elements, transform_3d_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateEffectGroup(
        IDCompositionDesktopDevice *iface, IDCompositionEffectGroup **effect_group)
{
    FIXME("iface %p, effect_group %p stub!\n", iface, effect_group);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateRectangleClip(
        IDCompositionDesktopDevice *iface, IDCompositionRectangleClip **clip)
{
    FIXME("iface %p, clip %p stub!\n", iface, clip);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_desktop_device_CreateAnimation(
        IDCompositionDesktopDevice *iface, IDCompositionAnimation **animation)
{
    FIXME("iface %p, animation %p stub!\n", iface, animation);
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
 * IDCompositionDevice3
 *
 * Chromium/WebView2 gates its entire DirectComposition presentation path
 * (ui/gl/direct_composition_support.cc: g_dcomp_device, DCLayerTree,
 * SwapChainPresenter) on a successful QI for IDCompositionDevice3 — the
 * effect factories themselves are never called there. The Device2-method
 * wrappers delegate to the desktop device; the 13 effect factories are
 * FIXME stubs until a caller actually needs them.
 * ===================================================================== */

static HRESULT STDMETHODCALLTYPE dcomp_device3_QueryInterface(
        IDCompositionDevice3 *iface, REFIID iid, void **out)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_device_QueryInterface(&device->IDCompositionDevice_iface, iid, out);
}

static ULONG STDMETHODCALLTYPE dcomp_device3_AddRef(IDCompositionDevice3 *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_device_AddRef(&device->IDCompositionDevice_iface);
}

static ULONG STDMETHODCALLTYPE dcomp_device3_Release(IDCompositionDevice3 *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_device_Release(&device->IDCompositionDevice_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_Commit(IDCompositionDevice3 *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_Commit(&device->IDCompositionDesktopDevice_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_WaitForCommitCompletion(
        IDCompositionDevice3 *iface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_WaitForCommitCompletion(&device->IDCompositionDesktopDevice_iface);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_GetFrameStatistics(
        IDCompositionDevice3 *iface, DCOMPOSITION_FRAME_STATISTICS *statistics)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_GetFrameStatistics(&device->IDCompositionDesktopDevice_iface,
            statistics);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateVisual(
        IDCompositionDevice3 *iface, IDCompositionVisual2 **visual)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_CreateVisual(&device->IDCompositionDesktopDevice_iface, visual);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateSurfaceFactory(
        IDCompositionDevice3 *iface, IUnknown *rendering_device,
        IDCompositionSurfaceFactory **surface_factory)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_CreateSurfaceFactory(&device->IDCompositionDesktopDevice_iface,
            rendering_device, surface_factory);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateSurface(
        IDCompositionDevice3 *iface, UINT width, UINT height,
        DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionSurface **surface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_CreateSurface(&device->IDCompositionDesktopDevice_iface,
            width, height, pixel_format, alpha_mode, surface);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateVirtualSurface(
        IDCompositionDevice3 *iface, UINT width, UINT height,
        DXGI_FORMAT pixel_format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_CreateVirtualSurface(&device->IDCompositionDesktopDevice_iface,
            width, height, pixel_format, alpha_mode, surface);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateTranslateTransform(
        IDCompositionDevice3 *iface, IDCompositionTranslateTransform **transform)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_CreateTranslateTransform(&device->IDCompositionDesktopDevice_iface,
            transform);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateScaleTransform(
        IDCompositionDevice3 *iface, IDCompositionScaleTransform **transform)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_CreateScaleTransform(&device->IDCompositionDesktopDevice_iface,
            transform);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateRotateTransform(
        IDCompositionDevice3 *iface, IDCompositionRotateTransform **transform)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_CreateRotateTransform(&device->IDCompositionDesktopDevice_iface,
            transform);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateSkewTransform(
        IDCompositionDevice3 *iface, IDCompositionSkewTransform **transform)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_CreateSkewTransform(&device->IDCompositionDesktopDevice_iface,
            transform);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateMatrixTransform(
        IDCompositionDevice3 *iface, IDCompositionMatrixTransform **transform)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_CreateMatrixTransform(&device->IDCompositionDesktopDevice_iface,
            transform);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateTransformGroup(
        IDCompositionDevice3 *iface, IDCompositionTransform **transforms,
        UINT elements, IDCompositionTransform **transform_group)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_CreateTransformGroup(&device->IDCompositionDesktopDevice_iface,
            transforms, elements, transform_group);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateTranslateTransform3D(
        IDCompositionDevice3 *iface, IDCompositionTranslateTransform3D **transform_3d)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_CreateTranslateTransform3D(&device->IDCompositionDesktopDevice_iface,
            transform_3d);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateScaleTransform3D(
        IDCompositionDevice3 *iface, IDCompositionScaleTransform3D **transform_3d)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_CreateScaleTransform3D(&device->IDCompositionDesktopDevice_iface,
            transform_3d);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateRotateTransform3D(
        IDCompositionDevice3 *iface, IDCompositionRotateTransform3D **transform_3d)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_CreateRotateTransform3D(&device->IDCompositionDesktopDevice_iface,
            transform_3d);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateMatrixTransform3D(
        IDCompositionDevice3 *iface, IDCompositionMatrixTransform3D **transform_3d)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_CreateMatrixTransform3D(&device->IDCompositionDesktopDevice_iface,
            transform_3d);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateTransform3DGroup(
        IDCompositionDevice3 *iface, IDCompositionTransform3D **transforms_3d,
        UINT elements, IDCompositionTransform3D **transform_3d_group)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_CreateTransform3DGroup(&device->IDCompositionDesktopDevice_iface,
            transforms_3d, elements, transform_3d_group);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateEffectGroup(
        IDCompositionDevice3 *iface, IDCompositionEffectGroup **effect_group)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_CreateEffectGroup(&device->IDCompositionDesktopDevice_iface,
            effect_group);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateRectangleClip(
        IDCompositionDevice3 *iface, IDCompositionRectangleClip **clip)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_CreateRectangleClip(&device->IDCompositionDesktopDevice_iface, clip);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateAnimation(
        IDCompositionDevice3 *iface, IDCompositionAnimation **animation)
{
    struct dcomp_device *device = impl_from_IDCompositionDevice3(iface);
    return dcomp_desktop_device_CreateAnimation(&device->IDCompositionDesktopDevice_iface, animation);
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateGaussianBlurEffect(
        IDCompositionDevice3 *iface, IDCompositionGaussianBlurEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateBrightnessEffect(
        IDCompositionDevice3 *iface, IDCompositionBrightnessEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateColorMatrixEffect(
        IDCompositionDevice3 *iface, IDCompositionColorMatrixEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateShadowEffect(
        IDCompositionDevice3 *iface, IDCompositionShadowEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateHueRotationEffect(
        IDCompositionDevice3 *iface, IDCompositionHueRotationEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateSaturationEffect(
        IDCompositionDevice3 *iface, IDCompositionSaturationEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateTurbulenceEffect(
        IDCompositionDevice3 *iface, IDCompositionTurbulenceEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateLinearTransferEffect(
        IDCompositionDevice3 *iface, IDCompositionLinearTransferEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateTableTransferEffect(
        IDCompositionDevice3 *iface, IDCompositionTableTransferEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateCompositeEffect(
        IDCompositionDevice3 *iface, IDCompositionCompositeEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateBlendEffect(
        IDCompositionDevice3 *iface, IDCompositionBlendEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateArithmeticCompositeEffect(
        IDCompositionDevice3 *iface, IDCompositionArithmeticCompositeEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dcomp_device3_CreateAffineTransform2DEffect(
        IDCompositionDevice3 *iface, IDCompositionAffineTransform2DEffect **effect)
{
    FIXME("iface %p, effect %p stub!\n", iface, effect);
    if (effect)
        *effect = NULL;
    return E_NOTIMPL;
}

static const IDCompositionDevice3Vtbl dcomp_device3_vtbl =
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
    object->IDCompositionDevice3_iface.lpVtbl = &dcomp_device3_vtbl;
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
