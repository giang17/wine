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
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC windowed_fullscreen_desc = {0};
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

    if (!fullscreen_desc || !dxgi_validate_swapchain_fullscreen_desc(fullscreen_desc))
    {
        if (fullscreen_desc)
            windowed_fullscreen_desc = *fullscreen_desc;
        windowed_fullscreen_desc.Windowed = TRUE;
        fullscreen_desc = &windowed_fullscreen_desc;
    }

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

/* DCOMP_REBLIT_TIMER_ID / DCOMP_POPUP_REBLIT_TIMER_ID moved to dxgi_private.h
 * so d3d11_swapchain_Release() can KillTimer them on teardown. */

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
        else
        {
            /* Without a DC the composition content stays in its buffer and the
             * window keeps whatever overwrote it -- the symptom is a stale or
             * blank area that no repaint fixes. */
            static unsigned int no_dc_count;

            if (++no_dc_count <= 5 || !(no_dc_count % 200))
                FIXME("Re-blit skipped #%u: GetDC failed on hwnd %p %ux%u reason=%s.\n",
                        no_dc_count, hwnd, w, h, reason);
        }
    }
    else
    {
        /* Three reports per process was nothing for a fault that persists: from
         * minute ten on the log said the problem had stopped.  Same throttle as
         * the success path above. */
        static unsigned int null_count;

        if (++null_count <= 5 || !(null_count % 200))
            FIXME("Re-blit skipped #%u: hwnd %p comp_dc=%p dims=%#Ix reason=%s.\n",
                    null_count, hwnd, comp_dc, (ULONG_PTR)dims, reason);
    }
}

/* Count of currently subclassed DComp target windows.  Used to distinguish
 * the first (primary) target from secondary targets (popups, tooltips).
 * Primary target gets full mode (timer + periodic Present), secondary targets
 * get lightweight popup mode.  Incremented after subclassing, decremented
 * in WM_NCDESTROY of both wndprocs.  Thread-safe via Interlocked ops. */
LONG dcomp_subclassed_target_count;

/* Process-wide signal that a DComp plugin GUI is currently hosted, published as
 * a property on the desktop window so winex11's set_style_hints can read it
 * (cross-module: dxgi is PE, winex11 is the Unix driver — a shared window
 * property is the idiomatic channel, same convention as __wine_dcomp_wnd_*).
 * Embedded in-plugin popups (ownerless WS_POPUP|WS_EX_TOOLWINDOW) anchor to the
 * host window, not to a DComp swapchain target, so the standalone discriminators
 * (popup_parent prop / anchor_is_dcomp) miss them and they stay UTILITY
 * (activatable) → focus war with the plugin's other popups → flicker (Trinity
 * IFX list, embedded).  This flag lets winex11 mark them DROPDOWN_MENU instead.
 * Set while count > 0, removed at 0 (SetProp with value 0 is not distinguishable
 * from "unset" — see the SetPropW (HANDLE)1/2 lesson).
 *
 * The property name carries this process's pid (__wine_dcomp_active_<pid:08x>)
 * and winex11 only honours the name built from the popup's own process (issue
 * 74): the signal is process-wide, but under a session-wide name it also
 * mis-typed borderless tool popups of unrelated non-DComp processes as
 * DROPDOWN_MENU.  The embedded case that motivated the flag (in-process VST3
 * popups, Trinity IFX under Windows Reaper) shares the plugin GUI's process,
 * so it keeps firing.  Per-process names also stop two DComp-hosting processes
 * from overwriting each other's count in a single shared property. */
static void dcomp_update_active_prop(void)
{
    static WCHAR name[40];
    LONG count = dcomp_subclassed_target_count;

    if (!name[0])
    {
        static const WCHAR hex[] = L"0123456789abcdef";
        DWORD pid = GetCurrentProcessId();
        WCHAR *p;
        int i;

        lstrcpyW(name, L"__wine_dcomp_active_");
        p = name + lstrlenW(name);
        for (i = 28; i >= 0; i -= 4) *p++ = hex[(pid >> i) & 0xf];
        *p = 0;
    }
    if (count > 0)
        SetPropW(GetDesktopWindow(), name, (HANDLE)(LONG_PTR)count);
    else
        RemovePropW(GetDesktopWindow(), name);
}

/* Popup transient-parent stack: the open DComp targets in open order
 * [main, settings, dropdown, ...]. A new popup's transient_for anchor is the
 * current top (the previously-opened target) — yielding the correct nested
 * hierarchy main <- settings <- dropdown — instead of the unreliable
 * get_active_window(), which (while the GL-presenting main window reactivates
 * itself) drags every popup behind the main window (Trinity standalone z-order
 * regression). Pushed after subclassing, removed in WM_NCDESTROY of both
 * wndprocs. UI-thread-serialized (the SET_TARGET message and NCDESTROY run on
 * the window's own thread) → no extra lock, like dcomp_subclassed_target_count.
 * Global for now (shared across plugins); per-plugin scoping ties into #11. */
#define DCOMP_POPUP_STACK_MAX 16
static HWND dcomp_popup_stack[DCOMP_POPUP_STACK_MAX];
static int dcomp_popup_stack_depth;

static HWND dcomp_popup_stack_top(void)
{
    return dcomp_popup_stack_depth > 0 ? dcomp_popup_stack[dcomp_popup_stack_depth - 1] : NULL;
}

static void dcomp_popup_stack_push(HWND hwnd)
{
    if (dcomp_popup_stack_depth < DCOMP_POPUP_STACK_MAX)
        dcomp_popup_stack[dcomp_popup_stack_depth++] = hwnd;
    else
        WARN("DComp popup stack full (%d), not pushing %p.\n", dcomp_popup_stack_depth, hwnd);
}

static void dcomp_popup_stack_remove(HWND hwnd)
{
    int i;

    for (i = 0; i < dcomp_popup_stack_depth; ++i)
    {
        if (dcomp_popup_stack[i] == hwnd)
        {
            memmove(&dcomp_popup_stack[i], &dcomp_popup_stack[i + 1],
                    (dcomp_popup_stack_depth - i - 1) * sizeof(*dcomp_popup_stack));
            --dcomp_popup_stack_depth;
            return;
        }
    }
}

/* Restore a parent's style if we OR-ed in WS_CLIPCHILDREN for this target
 * (see WM_WINE_DCOMP_SET_TARGET).  Keyed off the __wine_dcomp_parent_clip prop,
 * which is set only when we actually added the bit, so a parent that already had
 * WS_CLIPCHILDREN is left untouched.  Avoids a permanent host-window style change. */
static void dcomp_restore_parent_clip(HWND hwnd)
{
    HWND phwnd = (HWND)GetPropW(hwnd, L"__wine_dcomp_parent_clip");

    if (phwnd)
    {
        LONG pstyle = GetWindowLongW(phwnd, GWL_STYLE);
        if (pstyle & WS_CLIPCHILDREN)
            SetWindowLongW(phwnd, GWL_STYLE, pstyle & ~WS_CLIPCHILDREN);
        RemovePropW(hwnd, L"__wine_dcomp_parent_clip");
    }
}

/* Lightweight subclass for DComp popup windows (menus, tooltips).
 * Only blocks WM_ERASEBKGND and re-blits composition content on WM_PAINT.
 * NO timer, NO periodic Present â avoids dual-swapchain
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
            IDXGISwapChain4 *sc = (IDXGISwapChain4 *)GetPropW(hwnd, L"__wine_dcomp_swapchain");

            InterlockedDecrement(&dcomp_subclassed_target_count);
            dcomp_update_active_prop();
            dcomp_popup_stack_remove(hwnd);
            KillTimer(hwnd, DCOMP_POPUP_REBLIT_TIMER_ID);

            /* Before the popup window is destroyed, switch the swapchain back
             * to its composition window.  This drains the CS queue (no more
             * presents to the dead popup), releases the popup's DC, and
             * rebinds to a valid window.  Without this, the GL context stays
             * bound to the destroyed popup's drawable → page fault. */
            if (sc)
            {
                struct d3d11_swapchain *swapchain = d3d11_swapchain_from_IDXGISwapChain4(sc);
                WCHAR prop_name[64];
                HWND comp_wnd;

                /* Window dies first: clear the back-pointer so a later
                 * d3d11_swapchain_Release won't touch this (recycled) HWND. */
                swapchain->target_hwnd = NULL;

                swprintf(prop_name, ARRAY_SIZE(prop_name),
                        WINE_DCOMP_WND_PROP_FMT, GetCurrentProcessId(), (UINT_PTR)sc);
                comp_wnd = (HWND)GetPropW(GetDesktopWindow(), prop_name);

                wined3d_swapchain_set_prefer_gl_present(swapchain->wined3d_swapchain, FALSE);
                wined3d_swapchain_set_force_gdi_present(swapchain->wined3d_swapchain, TRUE);

                if (comp_wnd && IsWindow(comp_wnd))
                {
                    wined3d_swapchain_set_device_window(swapchain->wined3d_swapchain, comp_wnd);
                    FIXME("DComp popup %p destroyed, switched back to comp_wnd %p.\n",
                            hwnd, comp_wnd);
                }
                else
                {
                    FIXME("DComp popup %p destroyed, comp_wnd not found.\n", hwnd);
                }
            }

            dcomp_restore_parent_clip(hwnd);
            result = orig ? CallWindowProcW(orig, hwnd, msg, wparam, lparam)
                         : DefWindowProcW(hwnd, msg, wparam, lparam);
            RemovePropW(hwnd, L"__wine_dcomp_orig_wndproc");
            RemovePropW(hwnd, L"__wine_dcomp_subclass_proc");
            RemovePropW(hwnd, L"__wine_dcomp_swapchain");
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
            /* Forward WM_PAINT to the app's original wndproc FIRST.  DComp
             * plugins whose UI is paint-driven (EPROM's WebView2 loader only
             * re-renders on WM_SIZE/WM_PAINT, not on a timer) otherwise never
             * get their invalidation callback: hover/click state changes call
             * InvalidateRect, the generated WM_PAINT was swallowed here, and
             * the UI stayed frozen until the next resize forced a WM_SIZE
             * redraw.  The app's paint may trigger a re-render + Present,
             * which updates the comp buffer as a side effect.  Its own
             * BeginPaint sends WM_ERASEBKGND, which this subclass still
             * suppresses (returns 1), so no background erase happens.
             *
             * Afterwards re-blit the comp buffer via GetDC (NOT BeginPaint:
             * the app's EndPaint already validated the update region, so a
             * second BeginPaint would return an empty clip region and eat
             * our blit) so the swapchain content stays on top of whatever
             * the app painted. */
            if (orig)
                CallWindowProcW(orig, hwnd, msg, wparam, lparam);
            else
                DefWindowProcW(hwnd, msg, wparam, lparam);

            {
                static unsigned int paint_count;
                ++paint_count;
                if (paint_count <= 5 || !(paint_count % 200))
                    FIXME("WM_PAINT #%u (forwarded): hwnd %p, comp_dc=%p.\n",
                            paint_count, hwnd,
                            (HDC)GetPropW(hwnd, L"__wine_dcomp_comp_dc"));
            }
            dcomp_reblit_comp_buffer(hwnd, "paint");
            ValidateRect(hwnd, NULL);
            return 0;
        }

        case WM_TIMER:
            if (wparam == DCOMP_REBLIT_TIMER_ID)
            {
                IDXGISwapChain4 *sc = (IDXGISwapChain4 *)GetPropW(hwnd, L"__wine_dcomp_swapchain");

                if (sc)
                {
                    /* Present(0,0) without dirty rects.  The expensive GPU
                     * readback + StretchBlt is skipped in swapchain_blit_gdi
                     * when the composition buffer is already up-to-date
                     * (no new rendering since the last Present1).  We still
                     * call Present so JUCE's SwapChain event loop gets its
                     * frame-ready signal — without it, input events stall. */
                    IDXGISwapChain4_Present(sc, 0, 0);

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
            IDXGISwapChain4 *sc = (IDXGISwapChain4 *)GetPropW(hwnd, L"__wine_dcomp_swapchain");
            InterlockedDecrement(&dcomp_subclassed_target_count);
            dcomp_update_active_prop();
            dcomp_popup_stack_remove(hwnd);
            KillTimer(hwnd, DCOMP_REBLIT_TIMER_ID);
            /* Window dies first: clear the back-pointer so a later
             * d3d11_swapchain_Release won't touch this (recycled) HWND. */
            if (sc)
                d3d11_swapchain_from_IDXGISwapChain4(sc)->target_hwnd = NULL;
            dcomp_restore_parent_clip(hwnd);
            result = orig ? CallWindowProcW(orig, hwnd, msg, wparam, lparam)
                         : DefWindowProcW(hwnd, msg, wparam, lparam);
            RemovePropW(hwnd, L"__wine_dcomp_orig_wndproc");
            RemovePropW(hwnd, L"__wine_dcomp_subclass_proc");
            RemovePropW(hwnd, L"__wine_dcomp_swapchain");
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
                 *
                 * Top-level popup windows (WS_POPUP, parent == Desktop) have an
                 * X11 whole_window, so GL can create a client_window drawable and
                 * use glXSwapBuffers directly — no GPU readback or CPU copies.
                 * win32u's get_window_unused_drawable() calls p_surface_create
                 * on-demand when wglMakeCurrent needs a drawable for the popup.
                 *
                 * Child windows (embedded plugins) have no whole_window, so GL
                 * swap would write to an invisible drawable.  Force GDI blit for
                 * those — it writes to the window surface via comp_dc. */
                {
                    HWND target_parent_toplevel = GetAncestor(target_hwnd, GA_PARENT);
                    BOOL is_toplevel = !target_parent_toplevel
                            || target_parent_toplevel == GetDesktopWindow();

                    wined3d_swapchain_set_device_window(swapchain->wined3d_swapchain, target_hwnd);
                    /* Remember the subclassed target for teardown in d3d11_swapchain_Release. */
                    swapchain->target_hwnd = target_hwnd;

                    if (is_toplevel)
                    {
                        wined3d_swapchain_set_force_gdi_present(swapchain->wined3d_swapchain, FALSE);
                        wined3d_swapchain_set_prefer_gl_present(swapchain->wined3d_swapchain, TRUE);
                        FIXME("DComp: target %p is top-level, using GL present (no GDI readback).\n",
                                target_hwnd);
                    }
                    else
                    {
                        wined3d_swapchain_set_force_gdi_present(swapchain->wined3d_swapchain, TRUE);
                    }
                }

                /* Set premultiplied alpha blending if the swapchain uses it.
                 * This makes GDI blit use AlphaBlend instead of StretchBlt,
                 * allowing proper compositing of transparent areas. */
                if (swapchain->alpha_mode == DXGI_ALPHA_MODE_PREMULTIPLIED)
                    wined3d_swapchain_set_premultiplied_alpha(swapchain->wined3d_swapchain, TRUE);

                /* Chromium binds composition swapchains to its "Intermediate D3D Window"
                 * (WS_EX_LAYERED|WS_EX_NOREDIRECTIONBITMAP|WS_EX_TRANSPARENT) without ever
                 * calling SetLayeredWindowAttributes or UpdateLayeredWindow.  On Windows the
                 * DWM shows the composition visual directly and such a window needs no GDI
                 * surface at all; here the composition buffer arrives by GDI blit, and an
                 * unattributed layered window has no working GDI display path (winex11 defers
                 * mapping layered windows until attributes arrive, and their redraws stay
                 * suppressed) — presents would execute and never reach the screen.  An opaque
                 * LWA_ALPHA is a no-op for a NOREDIRECTIONBITMAP window under Windows
                 * semantics and makes the window an ordinary opaque surface here.  Targets
                 * that already carry attributes (JUCE's DropShadower, UpdateLayeredWindow
                 * users) have a working path already and keep their alpha semantics.
                 *
                 * From shibco/ableton-linux patch 0041, written by shibco, where it fixes
                 * Ableton Live's Learn View.  Measured inert for Reaper/FL hosting here —
                 * Chromium's composition swapchains never reach this bind at all — and kept
                 * as cover for hosts that do bind them. */
                {
                    DWORD target_exstyle = GetWindowLongW(target_hwnd, GWL_EXSTYLE);

                    if ((target_exstyle & WS_EX_NOREDIRECTIONBITMAP)
                            || ((target_exstyle & WS_EX_LAYERED)
                                && !GetLayeredWindowAttributes(target_hwnd, NULL, NULL, NULL)))
                    {
                        if (SetLayeredWindowAttributes(target_hwnd, 0, 255, LWA_ALPHA))
                            TRACE("DComp: target %p exstyle %#lx was layered without attributes, "
                                    "set opaque LWA_ALPHA so composition blits are visible.\n",
                                    target_hwnd, target_exstyle);
                        else
                            WARN("DComp: SetLayeredWindowAttributes failed for target %p, error %lu.\n",
                                    target_hwnd, GetLastError());
                    }
                }

                ShowWindow(hwnd, SW_HIDE);

                /* Detect popup mode via subclassed target count.
                 * First target (count == 0) gets full mode with timer +
                 * Present.  Subsequent targets get lightweight popup
                 * mode to avoid dual-swapchain interference.
                 *
                 * NB: this is a deliberate process-global heuristic — "the first
                 * DComp target created in the process is the plugin's main window,
                 * later ones are its popups/menus".  It is order-dependent and in
                 * principle a second unrelated plugin instance could influence the
                 * mode assignment, but it holds for the embedded single-plugin
                 * hosting we target and is empirically validated across all tested
                 * plugins.  A per-target discriminator (e.g. by window style/owner)
                 * would be less order-sensitive but is render-critical: a main
                 * window misclassified as a popup loses its present timer.  Do not
                 * swap the discriminator without a full plugin re-test.  The popup
                 * *stacking* (transient anchor) is handled separately via the popup
                 * stack below, not by this count. */
                {
                    BOOL is_popup_mode = FALSE;
                    LONG target_style = GetWindowLongW(target_hwnd, GWL_STYLE);
                    HWND target_parent = GetParent(target_hwnd);
                    /* Transient anchor for a new popup = the previously-opened
                     * target (top of the popup stack). */
                    HWND popup_parent = dcomp_popup_stack_top();

                    FIXME("DComp popup-detect: target %p style=0x%08lx parent=%p count=%ld.\n",
                            target_hwnd, (unsigned long)target_style, target_parent,
                            dcomp_subclassed_target_count);

                    if (dcomp_subclassed_target_count > 0)
                        is_popup_mode = TRUE;

                    /* Never chain a second subclass onto ourselves.  A resize makes
                     * the app recreate its composition swapchain, so we land here
                     * again for a window we already subclassed; SetWindowLongPtrW
                     * would then hand back our own wndproc and we would store it as
                     * the "original" one, so CallWindowProcW calls us forever.  The
                     * WM_NCHITTEST flood of a resize exhausts the 1 MB stack of the
                     * host's main thread within a few dozen mouse moves and kills
                     * the process. */
                    {
                        WNDPROC installed = (WNDPROC)GetWindowLongPtrW(target_hwnd, GWLP_WNDPROC);

                        /* dcomp subclasses composition targets as well, from another
                         * DLL, so a pointer comparison against our own procedures
                         * never recognises it.  Both modules publish the procedure
                         * they installed in __wine_dcomp_subclass_proc; if that is
                         * what sits on the window and we have already subclassed it
                         * once, ours is still in the chain and chaining again would
                         * close a ring - ours would call dcomp's, which calls the
                         * procedure it saved (ours) right back (issue 101). */
                        if (installed == dcomp_popup_wndproc || installed == dcomp_target_wndproc
                                || (installed
                                    && installed == (WNDPROC)GetPropW(target_hwnd, L"__wine_dcomp_subclass_proc")
                                    && GetPropW(target_hwnd, L"__wine_dcomp_orig_wndproc")))
                        {
                            FIXME("DComp: target %p already subclassed (wndproc %p), not chaining again.\n",
                                    target_hwnd, installed);
                            goto dcomp_subclass_done;
                        }
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
                            /* Publish what we installed so dcomp recognises the
                             * subclass as ours (issue 101). */
                            SetPropW(target_hwnd, L"__wine_dcomp_subclass_proc", (HANDLE)dcomp_popup_wndproc);
                            SetPropW(target_hwnd, L"__wine_dcomp_swapchain", (HANDLE)iface);
                            /* Anchor transient_for at the previously-opened target
                             * (read by winex11 set_style_hints) instead of
                             * get_active_window → nested hierarchy, no main-window pull. */
                            SetPropW(target_hwnd, L"__wine_dcomp_popup_parent", (HANDLE)popup_parent);
                            SetTimer(target_hwnd, DCOMP_POPUP_REBLIT_TIMER_ID, 200, NULL);
                            InterlockedIncrement(&dcomp_subclassed_target_count);
                            dcomp_update_active_prop();
                            dcomp_popup_stack_push(target_hwnd);
                            FIXME("DComp POPUP mode: target %p, orig wndproc %p, sc=%p, transient->%p (reblit timer 200ms).\n",
                                    target_hwnd, orig, iface, popup_parent);
                        }
                        else
                        {
                            FIXME("DComp popup: SetWindowLongPtrW FAILED for target %p, err %lu.\n",
                                    target_hwnd, GetLastError());
                        }
                    }
                    else
                    {
                        /* Full mode: subclass + timer + periodic Present for main window */
                        WNDPROC orig = (WNDPROC)SetWindowLongPtrW(target_hwnd, GWLP_WNDPROC,
                                (LONG_PTR)dcomp_target_wndproc);
                        if (orig)
                        {
                            SetPropW(target_hwnd, L"__wine_dcomp_orig_wndproc", (HANDLE)orig);
                            /* Publish what we installed so dcomp recognises the
                             * subclass as ours (issue 101). */
                            SetPropW(target_hwnd, L"__wine_dcomp_subclass_proc", (HANDLE)dcomp_target_wndproc);
                            SetPropW(target_hwnd, L"__wine_dcomp_swapchain", (HANDLE)iface);
                            SetTimer(target_hwnd, DCOMP_REBLIT_TIMER_ID, 200, NULL);
                            InterlockedIncrement(&dcomp_subclassed_target_count);
                            dcomp_update_active_prop();
                            /* Main window: stack base for the first popup's transient anchor. */
                            dcomp_popup_stack_push(target_hwnd);
                            FIXME("DComp: subclassed target %p, orig wndproc %p, timer started, sc=%p.\n",
                                    target_hwnd, orig, iface);
                        }
                        else
                        {
                            FIXME("DComp: SetWindowLongPtrW FAILED for target %p, err %lu.\n",
                                    target_hwnd, GetLastError());
                        }
                    }

dcomp_subclass_done:
                    ;
                }

                /* Prevent black flickering between GL presents.  The subclassed
                 * wndproc already returns 1 for WM_ERASEBKGND on this window, so
                 * the class background brush is never painted here — no need to
                 * zero the (class-wide) GCLP_HBRBACKGROUND, which would also hit
                 * unrelated windows sharing the class.
                 *
                 * Make the parent clip this child during its own painting.  Only
                 * OR in WS_CLIPCHILDREN if the parent did not already have it, and
                 * record the parent on the target so the style is restored on
                 * teardown (avoids a permanent host-window style change). */
                {
                    HWND phwnd = GetParent(target_hwnd);
                    if (phwnd)
                    {
                        LONG pstyle = GetWindowLongW(phwnd, GWL_STYLE);
                        if (!(pstyle & WS_CLIPCHILDREN))
                        {
                            SetWindowLongW(phwnd, GWL_STYLE, pstyle | WS_CLIPCHILDREN);
                            SetPropW(target_hwnd, L"__wine_dcomp_parent_clip", (HANDLE)phwnd);
                        }
                    }
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
                WINE_DCOMP_WND_PROP_FMT, GetCurrentProcessId(), (UINT_PTR)*swapchain);
        SetPropW(GetDesktopWindow(), prop_name, (HANDLE)window);
        /* Store back-reference for WM_WINE_DCOMP_SET_TARGET handler */
        SetPropW(window, L"__wine_dcomp_swapchain", (HANDLE)*swapchain);
        /* Remember the composition window for teardown in d3d11_swapchain_Release. */
        d3d11_swapchain_from_IDXGISwapChain4((IDXGISwapChain4 *)*swapchain)->comp_wnd = window;
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
    IDXGIAdapter1 *adapter_object;
    HRESULT hr;

    FIXME("iface %p, iid %s, adapter %p semi-stub, returning a hardware adapter.\n",
            iface, debugstr_guid(iid), adapter);

    if (!adapter)
        return DXGI_ERROR_INVALID_CALL;

    if (FAILED(hr = dxgi_factory_EnumAdapters1(iface, 0, &adapter_object)))
        return hr;

    hr = IDXGIAdapter1_QueryInterface(adapter_object, iid, adapter);
    IDXGIAdapter1_Release(adapter_object);
    return hr;
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
