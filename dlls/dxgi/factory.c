/*
 * Copyright 2008 Henri Verbeet for CodeWeavers
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
 *
 */

#include "dxgi_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(dxgi);

static inline struct dxgi_factory *impl_from_IWineDXGIFactory(IWineDXGIFactory *iface)
{
    return CONTAINING_RECORD(iface, struct dxgi_factory, IWineDXGIFactory_iface);
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_QueryInterface(IWineDXGIFactory *iface, REFIID iid, void **out)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IWineDXGIFactory)
            || IsEqualGUID(iid, &IID_IDXGIFactory7)
            || IsEqualGUID(iid, &IID_IDXGIFactory6)
            || IsEqualGUID(iid, &IID_IDXGIFactory5)
            || IsEqualGUID(iid, &IID_IDXGIFactory4)
            || IsEqualGUID(iid, &IID_IDXGIFactory3)
            || IsEqualGUID(iid, &IID_IDXGIFactory2)
            || (factory->extended && IsEqualGUID(iid, &IID_IDXGIFactory1))
            || IsEqualGUID(iid, &IID_IDXGIFactory)
            || IsEqualGUID(iid, &IID_IDXGIObject)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        IUnknown_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));

    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dxgi_factory_AddRef(IWineDXGIFactory *iface)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);
    ULONG refcount = InterlockedIncrement(&factory->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE dxgi_factory_Release(IWineDXGIFactory *iface)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);
    ULONG refcount = InterlockedDecrement(&factory->refcount);

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
    {
        if (factory->device_window)
            DestroyWindow(factory->device_window);

        wined3d_decref(factory->wined3d);
        wined3d_private_store_cleanup(&factory->private_store);
        free(factory);
    }

    return refcount;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_SetPrivateData(IWineDXGIFactory *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return dxgi_set_private_data(&factory->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_SetPrivateDataInterface(IWineDXGIFactory *iface,
        REFGUID guid, const IUnknown *object)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);

    TRACE("iface %p, guid %s, object %p.\n", iface, debugstr_guid(guid), object);

    return dxgi_set_private_data_interface(&factory->private_store, guid, object);
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_GetPrivateData(IWineDXGIFactory *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return dxgi_get_private_data(&factory->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_GetParent(IWineDXGIFactory *iface, REFIID iid, void **parent)
{
    WARN("iface %p, iid %s, parent %p.\n", iface, debugstr_guid(iid), parent);

    *parent = NULL;

    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_EnumAdapters1(IWineDXGIFactory *iface,
        UINT adapter_idx, IDXGIAdapter1 **adapter)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);
    struct dxgi_adapter *adapter_object;
    UINT adapter_count;
    HRESULT hr;

    TRACE("iface %p, adapter_idx %u, adapter %p.\n", iface, adapter_idx, adapter);

    if (!adapter)
        return DXGI_ERROR_INVALID_CALL;

    wined3d_mutex_lock();
    adapter_count = wined3d_get_adapter_count(factory->wined3d);
    wined3d_mutex_unlock();

    if (adapter_idx >= adapter_count)
    {
        *adapter = NULL;
        return DXGI_ERROR_NOT_FOUND;
    }

    if (FAILED(hr = dxgi_adapter_create(factory, adapter_idx, &adapter_object)))
    {
        *adapter = NULL;
        return hr;
    }

    *adapter = (IDXGIAdapter1 *)&adapter_object->IWineDXGIAdapter_iface;

    TRACE("Returning adapter %p.\n", *adapter);

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_EnumAdapters(IWineDXGIFactory *iface,
        UINT adapter_idx, IDXGIAdapter **adapter)
{
    TRACE("iface %p, adapter_idx %u, adapter %p.\n", iface, adapter_idx, adapter);

    return dxgi_factory_EnumAdapters1(iface, adapter_idx, (IDXGIAdapter1 **)adapter);
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_MakeWindowAssociation(IWineDXGIFactory *iface,
        HWND window, UINT flags)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);

    TRACE("iface %p, window %p, flags %#x.\n", iface, window, flags);

    if (flags > DXGI_MWA_VALID)
        return DXGI_ERROR_INVALID_CALL;

    if (!window)
    {
        wined3d_unregister_windows(factory->wined3d);
        return S_OK;
    }

    if (!wined3d_register_window(factory->wined3d, window, NULL, flags))
        return E_FAIL;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_GetWindowAssociation(IWineDXGIFactory *iface, HWND *window)
{
    TRACE("iface %p, window %p.\n", iface, window);

    if (!window)
        return DXGI_ERROR_INVALID_CALL;

    /* The tests show that this always returns NULL for some unknown reason. */
    *window = NULL;

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_CreateSwapChain(IWineDXGIFactory *iface,
        IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **swapchain)
{
    struct dxgi_factory *factory = impl_from_IWineDXGIFactory(iface);
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen_desc;
    DXGI_SWAP_CHAIN_DESC1 swapchain_desc;

    TRACE("iface %p, device %p, desc %p, swapchain %p.\n", iface, device, desc, swapchain);

    if (!desc)
    {
        WARN("Invalid pointer.\n");
        return DXGI_ERROR_INVALID_CALL;
    }

    swapchain_desc.Width = desc->BufferDesc.Width;
    swapchain_desc.Height = desc->BufferDesc.Height;
    swapchain_desc.Format = desc->BufferDesc.Format;
    swapchain_desc.Stereo = FALSE;
    swapchain_desc.SampleDesc = desc->SampleDesc;
    swapchain_desc.BufferUsage = desc->BufferUsage;
    swapchain_desc.BufferCount = desc->BufferCount;
    swapchain_desc.Scaling = DXGI_SCALING_STRETCH;
    swapchain_desc.SwapEffect = desc->SwapEffect;
    swapchain_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swapchain_desc.Flags = desc->Flags;

    fullscreen_desc.RefreshRate = desc->BufferDesc.RefreshRate;
    fullscreen_desc.ScanlineOrdering = desc->BufferDesc.ScanlineOrdering;
    fullscreen_desc.Scaling = desc->BufferDesc.Scaling;
    fullscreen_desc.Windowed = desc->Windowed;

    return IWineDXGIFactory_CreateSwapChainForHwnd(&factory->IWineDXGIFactory_iface,
            device, desc->OutputWindow, &swapchain_desc, &fullscreen_desc, NULL,
            (IDXGISwapChain1 **)swapchain);
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_CreateSoftwareAdapter(IWineDXGIFactory *iface,
        HMODULE swrast, IDXGIAdapter **adapter)
{
    FIXME("iface %p, swrast %p, adapter %p stub!\n", iface, swrast, adapter);

    return E_NOTIMPL;
}

static BOOL STDMETHODCALLTYPE dxgi_factory_IsCurrent(IWineDXGIFactory *iface)
{
    static BOOL once = FALSE;

    if (!once++)
        FIXME("iface %p stub!\n", iface);
    else
        WARN("iface %p stub!\n", iface);

    return TRUE;
}

static BOOL STDMETHODCALLTYPE dxgi_factory_IsWindowedStereoEnabled(IWineDXGIFactory *iface)
{
    FIXME("iface %p stub!\n", iface);

    return FALSE;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_CreateSwapChainForHwnd(IWineDXGIFactory *iface,
        IUnknown *device, HWND window, const DXGI_SWAP_CHAIN_DESC1 *desc,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc,
        IDXGIOutput *output, IDXGISwapChain1 **swapchain)
{
    IWineDXGISwapChainFactory *swapchain_factory;
    ID3D12CommandQueue *command_queue;
    HRESULT hr;

    TRACE("iface %p, device %p, window %p, desc %p, fullscreen_desc %p, output %p, swapchain %p.\n",
            iface, device, window, desc, fullscreen_desc, output, swapchain);

    if (!device || !window || !desc || !swapchain)
    {
        WARN("Invalid pointer.\n");
        return DXGI_ERROR_INVALID_CALL;
    }

    if (desc->Stereo)
    {
        FIXME("Stereo swapchains are not supported.\n");
        return DXGI_ERROR_UNSUPPORTED;
    }

    if (!dxgi_validate_swapchain_desc(desc))
        return DXGI_ERROR_INVALID_CALL;

    if (output)
        FIXME("Ignoring output %p.\n", output);

    if (SUCCEEDED(IUnknown_QueryInterface(device, &IID_IWineDXGISwapChainFactory, (void **)&swapchain_factory)))
    {
        hr = IWineDXGISwapChainFactory_create_swapchain(swapchain_factory,
                (IDXGIFactory *)iface, window, desc, fullscreen_desc, output, swapchain);
        IWineDXGISwapChainFactory_Release(swapchain_factory);
        return hr;
    }

    if (SUCCEEDED(IUnknown_QueryInterface(device, &IID_ID3D12CommandQueue, (void **)&command_queue)))
    {
        hr = d3d12_swapchain_create(iface, command_queue, window, desc, fullscreen_desc, swapchain);
        ID3D12CommandQueue_Release(command_queue);
        return hr;
    }

    ERR("This is not the device we're looking for.\n");
    return DXGI_ERROR_UNSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_CreateSwapChainForCoreWindow(IWineDXGIFactory *iface,
        IUnknown *device, IUnknown *window, const DXGI_SWAP_CHAIN_DESC1 *desc,
        IDXGIOutput *output, IDXGISwapChain1 **swapchain)
{
    FIXME("iface %p, device %p, window %p, desc %p, output %p, swapchain %p stub!\n",
            iface, device, window, desc, output, swapchain);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_GetSharedResourceAdapterLuid(IWineDXGIFactory *iface,
        HANDLE resource, LUID *luid)
{
    FIXME("iface %p, resource %p, luid %p stub!\n", iface, resource, luid);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_RegisterOcclusionStatusWindow(IWineDXGIFactory *iface,
        HWND window, UINT message, DWORD *cookie)
{
    FIXME("iface %p, window %p, message %#x, cookie %p stub!\n",
            iface, window, message, cookie);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_RegisterStereoStatusEvent(IWineDXGIFactory *iface,
        HANDLE event, DWORD *cookie)
{
    FIXME("iface %p, event %p, cookie %p stub!\n", iface, event, cookie);

    return E_NOTIMPL;
}

static void STDMETHODCALLTYPE dxgi_factory_UnregisterStereoStatus(IWineDXGIFactory *iface, DWORD cookie)
{
    FIXME("iface %p, cookie %#lx stub!\n", iface, cookie);
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_RegisterStereoStatusWindow(IWineDXGIFactory *iface,
        HWND window, UINT message, DWORD *cookie)
{
    FIXME("iface %p, window %p, message %#x, cookie %p stub!\n",
            iface, window, message, cookie);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_RegisterOcclusionStatusEvent(IWineDXGIFactory *iface,
        HANDLE event, DWORD *cookie)
{
    FIXME("iface %p, event %p, cookie %p stub!\n", iface, event, cookie);

    return E_NOTIMPL;
}

static void STDMETHODCALLTYPE dxgi_factory_UnregisterOcclusionStatus(IWineDXGIFactory *iface, DWORD cookie)
{
    FIXME("iface %p, cookie %#lx stub!\n", iface, cookie);
}

#define WM_WINE_DCOMP_SET_TARGET (WM_USER + 0x100)
#define WM_WINE_DCOMP_SET_CHILD_MODE (WM_USER + 0x101)

static inline struct d3d11_swapchain *d3d11_swapchain_from_IDXGISwapChain4(IDXGISwapChain4 *iface)
{
    return CONTAINING_RECORD(iface, struct d3d11_swapchain, IDXGISwapChain4_iface);
}

#define DCOMP_REBLIT_TIMER_ID 0xDC01
#define DCOMP_POPUP_REBLIT_TIMER_ID 0xDC02

/* Re-blit composition content from persistent buffer to window.
 * Called on WM_PAINT, focus changes, timer tick, and other events
 * that may cause the window surface to be overwritten. */
static void dcomp_reblit_comp_buffer(HWND hwnd, const char *reason)
{
    HDC comp_dc = (HDC)GetPropW(hwnd, L"__wine_dcomp_comp_dc");
    LPARAM dims = (LPARAM)GetPropW(hwnd, L"__wine_dcomp_comp_size");

    if (comp_dc && dims)
    {
        unsigned int w = LOWORD(dims);
        unsigned int h = HIWORD(dims);
        HDC hdc = GetDC(hwnd);
        if (hdc)
        {
            static unsigned int reblit_count;
            ++reblit_count;
            if (reblit_count <= 5 || !(reblit_count % 200))
                FIXME("Re-blit #%u: hwnd %p %ux%u reason=%s.\n",
                        reblit_count, hwnd, w, h, reason);
            BitBlt(hdc, 0, 0, w, h, comp_dc, 0, 0, SRCCOPY);
            ReleaseDC(hwnd, hdc);
        }
    }
    else
    {
        static unsigned int null_count;
        ++null_count;
        if (null_count <= 3)
            FIXME("Re-blit skipped: hwnd %p comp_dc=%p dims=%#Ix reason=%s.\n",
                    hwnd, comp_dc, (ULONG_PTR)dims, reason);
    }
}

/* ── DComp micro-resize: popup detection via EnumThreadWindows ──
 *
 * JUCE popup menus are separate top-level windows with WS_POPUP style
 * (class "JUCE_*", title "menu" or empty for shadow/border windows).
 * A micro-resize on the parent would trigger handleMovedOrResized()
 * which dismisses these popups.  We enumerate the current thread's
 * windows before each micro-resize and skip it when any visible
 * popup-style window exists (excluding DComp infrastructure windows
 * excluding the micro-resize target itself via skip_hwnd). */

struct dcomp_popup_check
{
    HWND skip_hwnd;
    BOOL found;
};

static BOOL CALLBACK dcomp_check_popup_cb(HWND wnd, LPARAM lp)
{
    struct dcomp_popup_check *pc = (struct dcomp_popup_check *)lp;
    RECT r;
    LONG style;
    int w, h;

    if (wnd == pc->skip_hwnd || !IsWindowVisible(wnd))
        return TRUE;

    style = GetWindowLongW(wnd, GWL_STYLE);
    if (!(style & WS_POPUP))
        return TRUE;

    /* Skip windows with title bars (WS_CAPTION = WS_BORDER | WS_DLGFRAME).
     * These are regular application windows (e.g. Reaper FX editor), not
     * popup menus.  JUCE popup menus use bare WS_POPUP without WS_CAPTION. */
    if ((style & WS_CAPTION) == WS_CAPTION)
        return TRUE;

    /* NOTE: We previously skipped DComp windows here (checking
     * __wine_dcomp_swapchain), but Prophecy popup menus are also DComp
     * targets, so they were never detected as popups.  The other filters
     * (WS_POPUP, no WS_CAPTION, size, skip_hwnd) are sufficient — the
     * micro-resize target itself is excluded via skip_hwnd, and other
     * embedded DComp targets are WS_CHILD (not WS_POPUP). */

    if (!GetWindowRect(wnd, &r))
        return TRUE;

    w = r.right - r.left;
    h = r.bottom - r.top;

    /* JUCE popup menus are at least ~50x20; shadow/border windows are
     * narrow strips (12xN or Nx12) but still > 50 in one dimension.
     * Infrastructure helper windows are tiny (1x1 or 113x2). */
    if (w > 50 && h > 20)
    {
        pc->found = TRUE;
        return FALSE; /* stop enumeration */
    }

    return TRUE;
}

/* Check if another DComp-subclassed target already exists in this thread.
 * Used to distinguish primary targets (main plugin window) from secondary
 * targets (popup menus, tooltips) that should use lightweight mode. */
struct dcomp_existing_target_check
{
    HWND skip_hwnd;
    BOOL found;
};

static BOOL CALLBACK dcomp_check_existing_target_cb(HWND wnd, LPARAM lp)
{
    struct dcomp_existing_target_check *ec = (struct dcomp_existing_target_check *)lp;
    if (wnd == ec->skip_hwnd)
        return TRUE;
    if (GetPropW(wnd, L"__wine_dcomp_swapchain"))
    {
        ec->found = TRUE;
        return FALSE;
    }
    return TRUE;
}

/* Lightweight subclass for DComp popup windows (menus, tooltips).
 * Only blocks WM_ERASEBKGND and re-blits composition content on WM_PAINT.
 * NO timer, NO micro-resize, NO periodic Present â avoids dual-swapchain
 * interference that causes main plugin window to flicker. */
static LRESULT CALLBACK dcomp_popup_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    WNDPROC orig = (WNDPROC)GetPropW(hwnd, L"__wine_dcomp_orig_wndproc");

    switch (msg)
    {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC comp_dc = (HDC)GetPropW(hwnd, L"__wine_dcomp_comp_dc");
            LPARAM dims = (LPARAM)GetPropW(hwnd, L"__wine_dcomp_comp_size");

            BeginPaint(hwnd, &ps);
            if (comp_dc && dims)
            {
                unsigned int w = LOWORD(dims);
                unsigned int h = HIWORD(dims);
                BitBlt(ps.hdc, 0, 0, w, h, comp_dc, 0, 0, SRCCOPY);
            }
            EndPaint(hwnd, &ps);
            ValidateRect(hwnd, NULL);
            return 0;
        }

        case WM_TIMER:
            if (wparam == DCOMP_POPUP_REBLIT_TIMER_ID)
            {
                /* Lightweight reblit: just BitBlt from comp_dc, no Present,
                 * no swapchain interaction.  Keeps the window surface fresh
                 * so Expose events (e.g. tooltip close) show current content
                 * instead of stale frames. */
                dcomp_reblit_comp_buffer(hwnd, "popup-timer");
                return 0;
            }
            break;

        case WM_NCDESTROY:
        {
            LRESULT result;
            KillTimer(hwnd, DCOMP_POPUP_REBLIT_TIMER_ID);
            result = orig ? CallWindowProcW(orig, hwnd, msg, wparam, lparam)
                         : DefWindowProcW(hwnd, msg, wparam, lparam);
            RemovePropW(hwnd, L"__wine_dcomp_orig_wndproc");
            RemovePropW(hwnd, L"__wine_dcomp_comp_dc");
            RemovePropW(hwnd, L"__wine_dcomp_comp_size");
            return result;
        }
    }

    return orig ? CallWindowProcW(orig, hwnd, msg, wparam, lparam)
                : DefWindowProcW(hwnd, msg, wparam, lparam);
}

/* Subclass WndProc for composition target windows (plugin's HWND).
 * Blocks WM_ERASEBKGND and re-blits composition content on events
 * that can overwrite the window surface (paint, focus, expose).
 * Uses a periodic timer as fallback since WM_ACTIVATE does not
 * reach child windows when the top-level parent loses focus. */
static LRESULT CALLBACK dcomp_target_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    WNDPROC orig = (WNDPROC)GetPropW(hwnd, L"__wine_dcomp_orig_wndproc");

    switch (msg)
    {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC comp_dc = (HDC)GetPropW(hwnd, L"__wine_dcomp_comp_dc");
            LPARAM dims = (LPARAM)GetPropW(hwnd, L"__wine_dcomp_comp_size");

            BeginPaint(hwnd, &ps);
            if (comp_dc && dims)
            {
                unsigned int w = LOWORD(dims);
                unsigned int h = HIWORD(dims);
                BitBlt(ps.hdc, 0, 0, w, h, comp_dc, 0, 0, SRCCOPY);
                {
                    static unsigned int paint_count;
                    ++paint_count;
                    if (paint_count <= 5 || !(paint_count % 200))
                        FIXME("WM_PAINT #%u: hwnd %p %ux%u, comp_dc=%p.\n",
                                paint_count, hwnd, w, h, comp_dc);
                }
            }
            else
            {
                static unsigned int paint_null;
                ++paint_null;
                if (paint_null <= 3)
                    FIXME("WM_PAINT: no comp buffer for hwnd %p (dc=%p dims=%#Ix).\n",
                            hwnd, comp_dc, (ULONG_PTR)dims);
            }
            EndPaint(hwnd, &ps);
            ValidateRect(hwnd, NULL);
            return 0;
        }

        case WM_TIMER:
            if (wparam == DCOMP_REBLIT_TIMER_ID)
            {
                static unsigned int dcomp_resize_cooldown;
                IDXGISwapChain4 *sc = (IDXGISwapChain4 *)GetPropW(hwnd, L"__wine_dcomp_swapchain");

                if (dcomp_resize_cooldown > 0)
                    --dcomp_resize_cooldown;

                if (sc)
                {
                    /* Present(0,0) without dirty rects.  The expensive GPU
                     * readback + StretchBlt is skipped in swapchain_blit_gdi
                     * when the composition buffer is already up-to-date
                     * (no new rendering since the last Present1).  We still
                     * call Present so JUCE's SwapChain event loop gets its
                     * frame-ready signal — without it, input events stall. */
                    IDXGISwapChain4_Present(sc, 0, 0);

                    /* DComp stale-UI workaround (event-driven):  When the app
                     * rendered new content (Present1 with dirty rects set the
                     * __wine_dcomp_content_changed property), perform a micro-
                     * resize (shrink 1px then restore) on the target window.
                     * This triggers JUCE's handleResize() which marks all cached
                     * component images dirty and forces a full repaint — exactly
                     * what happens naturally at non-100% view sizes.
                     *
                     * Uses GetWindowRect (not GetClientRect) so that the 1px
                     * delta applies to the window frame, not client area — this
                     * prevents cumulative shrinkage on standalone windows that
                     * have title bars and borders.
                     *
                     * A cooldown (3 ticks = 600ms) prevents excessive repaints
                     * when the app renders continuously. */
                    if (GetPropW(hwnd, L"__wine_dcomp_content_changed")
                            && dcomp_resize_cooldown == 0)
                    {
                        /* Skip micro-resize for standalone (top-level) windows.
                         * The stale-UI bug only manifests in VST3 plugins
                         * embedded in hosts (Reaper) at 100% view size.
                         * Standalone windows have no view-size scaling and
                         * the micro-resize causes a brief visual flash. */
                        HWND parent = GetParent(hwnd);
                        if (!parent || parent == GetDesktopWindow())
                        {
                            RemovePropW(hwnd, L"__wine_dcomp_content_changed");
                        }
                        else
                        {
                        struct dcomp_popup_check pc;
                        pc.skip_hwnd = hwnd;
                        pc.found = FALSE;
                        EnumThreadWindows(GetCurrentThreadId(), dcomp_check_popup_cb, (LPARAM)&pc);

                        if (pc.found)
                        {
                            /* A popup menu is open — skip micro-resize to avoid
                             * dismissing it.  The property stays set and will be
                             * consumed on the next timer tick after the popup
                             * closes.  See dcomp_check_popup_cb for details. */
                            TRACE("DComp micro-resize deferred: popup window active.\n");
                        }
                        else
                        {
                            RECT wr;
                            RemovePropW(hwnd, L"__wine_dcomp_content_changed");
                            if (GetWindowRect(hwnd, &wr))
                            {
                                int w = wr.right - wr.left;
                                int h = wr.bottom - wr.top;
                                if (w > 1 && h > 1)
                                {
                                    /* Suppress X11 ConfigureWindow for the shrink call.
                                     * The property tells the X11 driver to skip the
                                     * XReconfigureWMWindow so the 1px shrink is never
                                     * visible.  Win32 state (wineserver) still updates,
                                     * so JUCE sees the size change via GetWindowRect. */
                                    SetPropW(hwnd, L"__wine_dcomp_skip_x11_config", (HANDLE)1);
                                    SetWindowPos(hwnd, NULL, 0, 0, w - 1, h,
                                            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                                    RemovePropW(hwnd, L"__wine_dcomp_skip_x11_config");
                                    SetWindowPos(hwnd, NULL, 0, 0, w, h,
                                            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                                    dcomp_resize_cooldown = 3;
                                    TRACE("DComp micro-resize pulse: hwnd %p %dx%d (cooldown 3).\n",
                                            hwnd, w, h);
                                }
                            }
                        }
                        } /* else (embedded child window) */
                    }
                }
                else
                {
                    dcomp_reblit_comp_buffer(hwnd, "timer");
                }
                return 0;
            }
            break;

        case WM_WINDOWPOSCHANGED:
        {
            const WINDOWPOS *wp = (const WINDOWPOS *)lparam;
            LRESULT result = orig ? CallWindowProcW(orig, hwnd, msg, wparam, lparam)
                                  : DefWindowProcW(hwnd, msg, wparam, lparam);
            /* Only reblit when the window was actually moved, resized, or shown.
             * Pure Z-order changes (e.g. popup menu appearing above) do not
             * require a reblit and would compete with Timer Present, causing
             * visible flickering (Prophecy hamburger menu hover). */
            if (wp && (!(wp->flags & SWP_NOMOVE) || !(wp->flags & SWP_NOSIZE)
                    || (wp->flags & SWP_SHOWWINDOW)))
            {
                dcomp_reblit_comp_buffer(hwnd, "windowposchanged");
            }
            return result;
        }

        case WM_SHOWWINDOW:
        case WM_ACTIVATE:
        case WM_CHILDACTIVATE:
        case WM_MOUSEACTIVATE:
        {
            LRESULT result = orig ? CallWindowProcW(orig, hwnd, msg, wparam, lparam)
                                  : DefWindowProcW(hwnd, msg, wparam, lparam);
            dcomp_reblit_comp_buffer(hwnd, msg == WM_ACTIVATE ? "activate" :
                    msg == WM_SHOWWINDOW ? "showwindow" :
                    msg == WM_CHILDACTIVATE ? "childactivate" : "mouseactivate");
            return result;
        }

        case WM_NCDESTROY:
        {
            LRESULT result;
            KillTimer(hwnd, DCOMP_REBLIT_TIMER_ID);
            result = orig ? CallWindowProcW(orig, hwnd, msg, wparam, lparam)
                         : DefWindowProcW(hwnd, msg, wparam, lparam);
            RemovePropW(hwnd, L"__wine_dcomp_orig_wndproc");
            RemovePropW(hwnd, L"__wine_dcomp_comp_dc");
            RemovePropW(hwnd, L"__wine_dcomp_comp_size");
            return result;
        }
    }

    return orig ? CallWindowProcW(orig, hwnd, msg, wparam, lparam)
                : DefWindowProcW(hwnd, msg, wparam, lparam);
}

static LRESULT CALLBACK dcomp_swapchain_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_WINE_DCOMP_SET_TARGET:
        {
            HWND target_hwnd = (HWND)lparam;
            IDXGISwapChain4 *iface = (IDXGISwapChain4 *)GetPropW(hwnd, L"__wine_dcomp_swapchain");
            if (iface && target_hwnd)
            {
                struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);
                struct wined3d_swapchain_desc wined3d_desc;
                RECT rc;

                wined3d_swapchain_get_desc(swapchain->wined3d_swapchain, &wined3d_desc);
                {
                    RECT wr;
                    HWND phwnd = GetParent(target_hwnd);
                    GetClientRect(target_hwnd, &rc);
                    GetWindowRect(target_hwnd, &wr);
                    FIXME("DComp: target %p client=%ldx%ld screen=(%ld,%ld)-(%ld,%ld)"
                            " parent=%p sc=%ux%u.\n",
                            target_hwnd, rc.right, rc.bottom,
                            wr.left, wr.top, wr.right, wr.bottom,
                            phwnd, wined3d_desc.backbuffer_width,
                            wined3d_desc.backbuffer_height);
                    if (phwnd)
                    {
                        RECT pr;
                        GetWindowRect(phwnd, &pr);
                        FIXME("DComp: parent %p screen=(%ld,%ld)-(%ld,%ld).\n",
                                phwnd, pr.left, pr.top, pr.right, pr.bottom);
                    }
                }

                /* Switch swapchain device window from comp_wnd to target_hwnd.
                 * The GL context rebinds lazily via context_gl_update_window.
                 * Force GDI blit for present: target_hwnd has no X11 whole_window,
                 * so GL swap would write to an invisible drawable. GDI blit writes
                 * to the window surface, which persists through parent repaints. */
                wined3d_swapchain_set_device_window(swapchain->wined3d_swapchain, target_hwnd);
                wined3d_swapchain_set_force_gdi_present(swapchain->wined3d_swapchain, TRUE);

                /* Set premultiplied alpha blending if the swapchain uses it.
                 * This makes GDI blit use AlphaBlend instead of StretchBlt,
                 * allowing proper compositing of transparent areas. */
                if (swapchain->alpha_mode == DXGI_ALPHA_MODE_PREMULTIPLIED)
                    wined3d_swapchain_set_premultiplied_alpha(swapchain->wined3d_swapchain, TRUE);

                ShowWindow(hwnd, SW_HIDE);

                /* Detect popup mode: any DComp target where another window
                 * with __wine_dcomp_swapchain already exists in this thread.
                 * This catches ALL secondary targets (popups, tooltips, dialogs
                 * with WS_CAPTION like Pianoteq Options) because the composition
                 * infrastructure window (from CreateSwapChainForComposition) gets
                 * __wine_dcomp_swapchain before the real target is subclassed.
                 * Popup mode = lightweight wndproc (no timer, no micro-resize)
                 * to avoid dual-swapchain interference that causes flicker. */
                {
                    BOOL is_popup_mode = FALSE;
                    LONG target_style = GetWindowLongW(target_hwnd, GWL_STYLE);
                    HWND target_parent = GetParent(target_hwnd);

                    FIXME("DComp popup-detect: target %p style=0x%08lx parent=%p.\n",
                            target_hwnd, (unsigned long)target_style, target_parent);

                    {
                        struct dcomp_existing_target_check ec;
                        ec.skip_hwnd = target_hwnd;
                        ec.found = FALSE;
                        EnumThreadWindows(GetCurrentThreadId(),
                                dcomp_check_existing_target_cb, (LPARAM)&ec);
                        if (ec.found)
                            is_popup_mode = TRUE;
                    }

                    if (is_popup_mode)
                    {
                        /* Lightweight popup mode: subclass with minimal wndproc
                         * (WM_ERASEBKGND + WM_PAINT only), no timer. */
                        WNDPROC orig = (WNDPROC)SetWindowLongPtrW(target_hwnd, GWLP_WNDPROC,
                                (LONG_PTR)dcomp_popup_wndproc);
                        if (orig)
                        {
                            SetPropW(target_hwnd, L"__wine_dcomp_orig_wndproc", (HANDLE)orig);
                            SetPropW(target_hwnd, L"__wine_dcomp_swapchain", (HANDLE)iface);
                            SetTimer(target_hwnd, DCOMP_POPUP_REBLIT_TIMER_ID, 200, NULL);
                            FIXME("DComp POPUP mode: target %p, orig wndproc %p, sc=%p (reblit timer 200ms).\n",
                                    target_hwnd, orig, iface);
                        }
                        else
                        {
                            FIXME("DComp popup: SetWindowLongPtrW FAILED for target %p, err %lu.\n",
                                    target_hwnd, GetLastError());
                        }
                    }
                    else
                    {
                        /* Full mode: subclass + timer + micro-resize for main window */
                        WNDPROC orig = (WNDPROC)SetWindowLongPtrW(target_hwnd, GWLP_WNDPROC,
                                (LONG_PTR)dcomp_target_wndproc);
                        if (orig)
                        {
                            SetPropW(target_hwnd, L"__wine_dcomp_orig_wndproc", (HANDLE)orig);
                            SetPropW(target_hwnd, L"__wine_dcomp_swapchain", (HANDLE)iface);
                            SetTimer(target_hwnd, DCOMP_REBLIT_TIMER_ID, 200, NULL);
                            FIXME("DComp: subclassed target %p, orig wndproc %p, timer started, sc=%p.\n",
                                    target_hwnd, orig, iface);
                        }
                        else
                        {
                            FIXME("DComp: SetWindowLongPtrW FAILED for target %p, err %lu.\n",
                                    target_hwnd, GetLastError());
                        }
                    }
                }

                /* Prevent black flickering between GL presents */
                SetClassLongPtrW(target_hwnd, GCLP_HBRBACKGROUND, 0);
                {
                    HWND phwnd = GetParent(target_hwnd);
                    if (phwnd)
                        SetWindowLongW(phwnd, GWL_STYLE,
                                GetWindowLongW(phwnd, GWL_STYLE) | WS_CLIPCHILDREN);
                }
                ValidateRect(target_hwnd, NULL);

                GetClientRect(target_hwnd, &rc);
                FIXME("DComp: target %p now %ldx%ld, swapchain switched.\n",
                        target_hwnd, rc.right, rc.bottom);
            }
            return 0;
        }

        case WM_WINE_DCOMP_SET_CHILD_MODE:
        {
            IDXGISwapChain4 *iface = (IDXGISwapChain4 *)GetPropW(hwnd, L"__wine_dcomp_swapchain");
            if (iface)
            {
                struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(iface);

                /* Enable GDI present so child renders into a comp buffer
                 * (CreateDIBSection) instead of GL-swapping to the hidden window.
                 * The root visual reads the child's comp_bits for compositing. */
                wined3d_swapchain_set_force_gdi_present(swapchain->wined3d_swapchain, TRUE);

                if (swapchain->alpha_mode == DXGI_ALPHA_MODE_PREMULTIPLIED)
                    wined3d_swapchain_set_premultiplied_alpha(swapchain->wined3d_swapchain, TRUE);

                /* Mark this window as a child visual's comp window */
                SetPropW(hwnd, L"__wine_dcomp_is_child", (HANDLE)(ULONG_PTR)1);

                FIXME("DComp child mode: hwnd %p, sc %p, alpha_mode %u.\n",
                        hwnd, iface, swapchain->alpha_mode);
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static BOOL dcomp_register_window_class(void)
{
    static BOOL registered;
    WNDCLASSW wc;

    if (registered)
        return TRUE;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = dcomp_swapchain_wndproc;
    wc.style = CS_OWNDC;
    wc.lpszClassName = L"WineDCompSwapchain";
    wc.hInstance = GetModuleHandleW(NULL);

    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        ERR("Failed to register WineDCompSwapchain class, error %lu.\n", GetLastError());
        return FALSE;
    }

    registered = TRUE;
    return TRUE;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_CreateSwapChainForComposition(IWineDXGIFactory *iface,
        IUnknown *device, const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIOutput *output, IDXGISwapChain1 **swapchain)
{
    WCHAR prop_name[64];
    HWND window;
    HRESULT hr;

    TRACE("iface %p, device %p, desc %p, output %p, swapchain %p.\n",
            iface, device, desc, output, swapchain);

    if (!device || !desc || !swapchain)
    {
        WARN("Invalid pointer.\n");
        return DXGI_ERROR_INVALID_CALL;
    }

    if (!dcomp_register_window_class())
        return E_FAIL;

    window = CreateWindowExW(WS_EX_NOPARENTNOTIFY,
            L"WineDCompSwapchain", L"DComp Swapchain",
            WS_CHILD, 0, 0, desc->Width, desc->Height,
            GetDesktopWindow(), NULL, GetModuleHandleW(NULL), NULL);
    if (!window)
    {
        ERR("Failed to create composition window, error %lu.\n", GetLastError());
        return E_FAIL;
    }

    hr = dxgi_factory_CreateSwapChainForHwnd(iface, device, window,
            desc, NULL, output, swapchain);
    if (SUCCEEDED(hr))
    {
        swprintf(prop_name, ARRAY_SIZE(prop_name),
                L"__wine_dcomp_wnd_%I64x", (UINT_PTR)*swapchain);
        SetPropW(GetDesktopWindow(), prop_name, (HANDLE)window);
        /* Store back-reference for WM_WINE_DCOMP_SET_TARGET handler */
        SetPropW(window, L"__wine_dcomp_swapchain", (HANDLE)*swapchain);
        FIXME("Created composition window %p (%ux%u) for swapchain %p.\n",
                window, desc->Width, desc->Height, *swapchain);
    }
    else
    {
        ERR("CreateSwapChainForHwnd failed, hr %#lx.\n", hr);
        DestroyWindow(window);
    }

    return hr;
}

static UINT STDMETHODCALLTYPE dxgi_factory_GetCreationFlags(IWineDXGIFactory *iface)
{
    FIXME("iface %p stub!\n", iface);

    return 0;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_EnumAdapterByLuid(IWineDXGIFactory *iface,
        LUID luid, REFIID iid, void **adapter)
{
    unsigned int adapter_index;
    DXGI_ADAPTER_DESC1 desc;
    IDXGIAdapter1 *adapter1;
    HRESULT hr;

    TRACE("iface %p, luid %08lx:%08lx, iid %s, adapter %p.\n",
            iface, luid.HighPart, luid.LowPart, debugstr_guid(iid), adapter);

    if (!adapter)
        return DXGI_ERROR_INVALID_CALL;

    adapter_index = 0;
    while ((hr = dxgi_factory_EnumAdapters1(iface, adapter_index, &adapter1)) == S_OK)
    {
        if (FAILED(hr = IDXGIAdapter1_GetDesc1(adapter1, &desc)))
        {
            WARN("Failed to get adapter %u desc, hr %#lx.\n", adapter_index, hr);
            ++adapter_index;
            continue;
        }

        if (desc.AdapterLuid.LowPart == luid.LowPart
                && desc.AdapterLuid.HighPart == luid.HighPart)
        {
            hr = IDXGIAdapter1_QueryInterface(adapter1, iid, adapter);
            IDXGIAdapter1_Release(adapter1);
            return hr;
        }

        IDXGIAdapter1_Release(adapter1);
        ++adapter_index;
    }
    if (hr != DXGI_ERROR_NOT_FOUND)
        WARN("Failed to enumerate adapters, hr %#lx.\n", hr);

    WARN("Adapter could not be found.\n");
    return DXGI_ERROR_NOT_FOUND;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_EnumWarpAdapter(IWineDXGIFactory *iface,
        REFIID iid, void **adapter)
{
    FIXME("iface %p, iid %s, adapter %p stub!\n", iface, debugstr_guid(iid), adapter);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_CheckFeatureSupport(IWineDXGIFactory *iface,
        DXGI_FEATURE feature, void *feature_data, UINT data_size)
{
    TRACE("iface %p, feature %#x, feature_data %p, data_size %u.\n",
            iface, feature, feature_data, data_size);

    switch (feature)
    {
        case DXGI_FEATURE_PRESENT_ALLOW_TEARING:
            if (data_size != sizeof(BOOL))
                return DXGI_ERROR_INVALID_CALL;
            *(BOOL *)feature_data = TRUE;
            return S_OK;

        default:
            WARN("Unsupported feature %#x.\n", feature);
            return DXGI_ERROR_INVALID_CALL;
    }
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_EnumAdapterByGpuPreference(IWineDXGIFactory *iface,
        UINT adapter_idx, DXGI_GPU_PREFERENCE gpu_preference, REFIID iid, void **adapter)
{
    IDXGIAdapter1 *adapter_object;
    HRESULT hr;

    TRACE("iface %p, adapter_idx %u, gpu_preference %#x, iid %s, adapter %p.\n",
            iface, adapter_idx, gpu_preference, debugstr_guid(iid), adapter);

    if (gpu_preference != DXGI_GPU_PREFERENCE_UNSPECIFIED)
        FIXME("Ignoring GPU preference %#x.\n", gpu_preference);

    if (FAILED(hr = dxgi_factory_EnumAdapters1(iface, adapter_idx, &adapter_object)))
        return hr;

    hr = IDXGIAdapter1_QueryInterface(adapter_object, iid, adapter);
    IDXGIAdapter1_Release(adapter_object);
    return hr;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_RegisterAdaptersChangedEvent(IWineDXGIFactory *iface,
        HANDLE event, DWORD *cookie)
{
    FIXME("iface %p, event %p, cookie %p stub!\n", iface, event, cookie);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE dxgi_factory_UnregisterAdaptersChangedEvent(IWineDXGIFactory *iface,
        DWORD cookie)
{
    FIXME("iface %p, cookie %#lx stub!\n", iface, cookie);

    return E_NOTIMPL;
}

static const struct IWineDXGIFactoryVtbl dxgi_factory_vtbl =
{
    dxgi_factory_QueryInterface,
    dxgi_factory_AddRef,
    dxgi_factory_Release,
    dxgi_factory_SetPrivateData,
    dxgi_factory_SetPrivateDataInterface,
    dxgi_factory_GetPrivateData,
    dxgi_factory_GetParent,
    dxgi_factory_EnumAdapters,
    dxgi_factory_MakeWindowAssociation,
    dxgi_factory_GetWindowAssociation,
    dxgi_factory_CreateSwapChain,
    dxgi_factory_CreateSoftwareAdapter,
    /* IDXGIFactory1 methods */
    dxgi_factory_EnumAdapters1,
    dxgi_factory_IsCurrent,
    /* IDXGIFactory2 methods */
    dxgi_factory_IsWindowedStereoEnabled,
    dxgi_factory_CreateSwapChainForHwnd,
    dxgi_factory_CreateSwapChainForCoreWindow,
    dxgi_factory_GetSharedResourceAdapterLuid,
    dxgi_factory_RegisterStereoStatusWindow,
    dxgi_factory_RegisterStereoStatusEvent,
    dxgi_factory_UnregisterStereoStatus,
    dxgi_factory_RegisterOcclusionStatusWindow,
    dxgi_factory_RegisterOcclusionStatusEvent,
    dxgi_factory_UnregisterOcclusionStatus,
    dxgi_factory_CreateSwapChainForComposition,
    /* IDXGIFactory3 methods */
    dxgi_factory_GetCreationFlags,
    /* IDXGIFactory4 methods */
    dxgi_factory_EnumAdapterByLuid,
    dxgi_factory_EnumWarpAdapter,
    /* IDXIGFactory5 methods */
    dxgi_factory_CheckFeatureSupport,
    /* IDXGIFactory6 methods */
    dxgi_factory_EnumAdapterByGpuPreference,
    /* IDXGIFactory7 methods */
    dxgi_factory_RegisterAdaptersChangedEvent,
    dxgi_factory_UnregisterAdaptersChangedEvent,
};

struct dxgi_factory *unsafe_impl_from_IDXGIFactory(IDXGIFactory *iface)
{
    IWineDXGIFactory *wine_factory;
    struct dxgi_factory *factory;
    HRESULT hr;

    if (!iface)
        return NULL;
    if (FAILED(hr = IDXGIFactory_QueryInterface(iface, &IID_IWineDXGIFactory, (void **)&wine_factory)))
    {
        ERR("Failed to get IWineDXGIFactory interface, hr %#lx.\n", hr);
        return NULL;
    }
    assert(wine_factory->lpVtbl == &dxgi_factory_vtbl);
    factory = CONTAINING_RECORD(wine_factory, struct dxgi_factory, IWineDXGIFactory_iface);
    IWineDXGIFactory_Release(wine_factory);
    return factory;
}

static HRESULT dxgi_factory_init(struct dxgi_factory *factory, BOOL extended)
{
    factory->IWineDXGIFactory_iface.lpVtbl = &dxgi_factory_vtbl;
    factory->refcount = 1;
    wined3d_private_store_init(&factory->private_store);

    wined3d_mutex_lock();
    factory->wined3d = wined3d_create(0);
    wined3d_mutex_unlock();
    if (!factory->wined3d)
    {
        wined3d_private_store_cleanup(&factory->private_store);
        return DXGI_ERROR_UNSUPPORTED;
    }

    factory->extended = extended;

    return S_OK;
}

HRESULT dxgi_factory_create(REFIID riid, void **factory, BOOL extended)
{
    struct dxgi_factory *object;
    HRESULT hr;

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    if (FAILED(hr = dxgi_factory_init(object, extended)))
    {
        WARN("Failed to initialize factory, hr %#lx.\n", hr);
        free(object);
        return hr;
    }

    TRACE("Created factory %p.\n", object);

    hr = IWineDXGIFactory_QueryInterface(&object->IWineDXGIFactory_iface, riid, factory);
    IWineDXGIFactory_Release(&object->IWineDXGIFactory_iface);
    return hr;
}

HWND dxgi_factory_get_device_window(struct dxgi_factory *factory)
{
    wined3d_mutex_lock();

    if (!factory->device_window)
    {
        if (!(factory->device_window = CreateWindowA("static", "DXGI device window",
                WS_DISABLED, 0, 0, 0, 0, NULL, NULL, NULL, NULL)))
        {
            wined3d_mutex_unlock();
            ERR("Failed to create a window.\n");
            return NULL;
        }
        SetWindowPos(factory->device_window, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        TRACE("Created device window %p for factory %p.\n", factory->device_window, factory);
    }

    wined3d_mutex_unlock();

    return factory->device_window;
}
