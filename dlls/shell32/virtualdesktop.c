/*
 * VirtualDesktopManager stub implementation
 *
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

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "objbase.h"

#include "wine/debug.h"
#include "shell32_main.h"

WINE_DEFAULT_DEBUG_CHANNEL(shell);

const GUID CLSID_VirtualDesktopManager =
        {0xaa509086, 0x5ca9, 0x4c25, {0x8f, 0x95, 0x58, 0x9d, 0x3c, 0x07, 0xb4, 0x8a}};
static const GUID IID_IVirtualDesktopManager =
        {0xa5cd92ff, 0x29be, 0x454c, {0x8d, 0x04, 0xd8, 0x28, 0x79, 0xfb, 0x3f, 0x1b}};

/* IVirtualDesktopManager vtable — not in Wine headers yet. */
typedef struct IVirtualDesktopManagerVtbl
{
    /* IUnknown */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(
            IUnknown *iface, REFIID riid, void **out);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IUnknown *iface);
    ULONG   (STDMETHODCALLTYPE *Release)(IUnknown *iface);
    /* IVirtualDesktopManager */
    HRESULT (STDMETHODCALLTYPE *IsWindowOnCurrentVirtualDesktop)(
            IUnknown *iface, HWND hwnd, BOOL *on_current);
    HRESULT (STDMETHODCALLTYPE *GetWindowDesktopId)(
            IUnknown *iface, HWND hwnd, GUID *desktop_id);
    HRESULT (STDMETHODCALLTYPE *MoveWindowToDesktop)(
            IUnknown *iface, HWND hwnd, REFGUID desktop_id);
} IVirtualDesktopManagerVtbl;

struct virtual_desktop_manager
{
    IVirtualDesktopManagerVtbl *lpVtbl;
    LONG ref;
};

static inline struct virtual_desktop_manager *impl_from_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct virtual_desktop_manager, lpVtbl);
}

static HRESULT STDMETHODCALLTYPE vdm_QueryInterface(IUnknown *iface, REFIID riid, void **out)
{
    struct virtual_desktop_manager *This = impl_from_IUnknown(iface);

    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(riid), out);

    if (!out)
        return E_POINTER;

    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IVirtualDesktopManager))
    {
        *out = &This->lpVtbl;
        IUnknown_AddRef(iface);
        return S_OK;
    }

    FIXME("(%p)->(%s, %p) not found\n", This, debugstr_guid(riid), out);
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE vdm_AddRef(IUnknown *iface)
{
    struct virtual_desktop_manager *This = impl_from_IUnknown(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p) ref=%lu\n", This, ref);
    return ref;
}

static ULONG STDMETHODCALLTYPE vdm_Release(IUnknown *iface)
{
    struct virtual_desktop_manager *This = impl_from_IUnknown(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p) ref=%lu\n", This, ref);
    if (!ref)
        free(This);
    return ref;
}

static HRESULT STDMETHODCALLTYPE vdm_IsWindowOnCurrentVirtualDesktop(
        IUnknown *iface, HWND hwnd, BOOL *on_current)
{
    TRACE("(%p)->(%p, %p) stub!\n", iface, hwnd, on_current);

    if (!on_current)
        return E_INVALIDARG;

    *on_current = TRUE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vdm_GetWindowDesktopId(
        IUnknown *iface, HWND hwnd, GUID *desktop_id)
{
    FIXME("(%p)->(%p, %p) stub!\n", iface, hwnd, desktop_id);

    if (!desktop_id)
        return E_INVALIDARG;

    memset(desktop_id, 0, sizeof(*desktop_id));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vdm_MoveWindowToDesktop(
        IUnknown *iface, HWND hwnd, REFGUID desktop_id)
{
    FIXME("(%p)->(%p, %s) stub!\n", iface, hwnd, debugstr_guid(desktop_id));
    return S_OK;
}

static const IVirtualDesktopManagerVtbl vdm_vtbl =
{
    vdm_QueryInterface,
    vdm_AddRef,
    vdm_Release,
    vdm_IsWindowOnCurrentVirtualDesktop,
    vdm_GetWindowDesktopId,
    vdm_MoveWindowToDesktop,
};

HRESULT WINAPI VirtualDesktopManager_Constructor(IUnknown *outer, REFIID riid, void **out)
{
    struct virtual_desktop_manager *object;
    HRESULT hr;

    TRACE("(%p, %s, %p)\n", outer, debugstr_guid(riid), out);

    if (outer)
        return CLASS_E_NOAGGREGATION;

    object = calloc(1, sizeof(*object));
    if (!object)
        return E_OUTOFMEMORY;

    object->lpVtbl = (IVirtualDesktopManagerVtbl *)&vdm_vtbl;
    object->ref = 1;

    hr = vdm_QueryInterface((IUnknown *)&object->lpVtbl, riid, out);
    vdm_Release((IUnknown *)&object->lpVtbl);
    return hr;
}
