/*
 * ID3D11Fence implementation with CPU timeline semantics
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

/* The fence timeline lives entirely on the CPU: ID3D11DeviceContext4::Signal
 * flushes the wined3d command stream and then advances the completed value
 * directly, without involving the GPU driver.  This matches how our DComp
 * composition pipeline consumes fences (CPU-synchronous readback), and is
 * sufficient for Chromium's D3DSharedFence usage, which only signals after
 * submitting work and waits before consuming it on another device. */

#include "d3d11_private.h"
#include "winternl.h"

WINE_DEFAULT_DEBUG_CHANNEL(d3d11);

struct d3d11_fence_waiter
{
    UINT64 value;
    HANDLE event;
};

struct d3d11_fence
{
    ID3D11Fence ID3D11Fence_iface;
    LONG refcount;

    struct wined3d_private_store private_store;
    ID3D11Device5 *device;
    D3D11_FENCE_FLAG flags;

    CRITICAL_SECTION cs;
    UINT64 completed;
    struct d3d11_fence_waiter *waiters;
    SIZE_T waiter_count, waiters_size;
};

/* Process-wide registry backing CreateSharedHandle/OpenSharedFence.  The
 * returned handle is a plain event object serving as a token; the registry
 * maps the underlying kernel object back to the fence.  Lookups compare
 * kernel object identity (NtCompareObjects), so handles duplicated with
 * DuplicateHandle - which Chromium does for every fence handle - still
 * resolve.  Sharing therefore works within one process only; entries hold
 * a reference on the fence and stay registered until process exit. */
struct d3d11_fence_share
{
    HANDLE token;               /* registry-owned event handle */
    struct d3d11_fence *fence;  /* strong reference */
};

static struct d3d11_fence_share *fence_shares;
static SIZE_T fence_share_count, fence_shares_size;

static CRITICAL_SECTION fence_share_cs;
static CRITICAL_SECTION_DEBUG fence_share_cs_debug =
{
    0, 0, &fence_share_cs,
    {&fence_share_cs_debug.ProcessLocksList, &fence_share_cs_debug.ProcessLocksList},
      0, 0, {(DWORD_PTR)(__FILE__ ": fence_share_cs")}
};
static CRITICAL_SECTION fence_share_cs = {&fence_share_cs_debug, -1, 0, 0, 0, 0};

static BOOL fence_array_reserve(void **elements, SIZE_T *capacity, SIZE_T count, SIZE_T size)
{
    SIZE_T max_capacity, new_capacity;
    void *new_elements;

    if (count <= *capacity)
        return TRUE;

    max_capacity = ~(SIZE_T)0 / size;
    if (count > max_capacity)
        return FALSE;

    new_capacity = max(1, *capacity);
    while (new_capacity < count && new_capacity <= max_capacity / 2)
        new_capacity *= 2;
    if (new_capacity < count)
        new_capacity = count;

    if (!(new_elements = realloc(*elements, new_capacity * size)))
        return FALSE;

    *elements = new_elements;
    *capacity = new_capacity;
    return TRUE;
}

static inline struct d3d11_fence *impl_from_ID3D11Fence(ID3D11Fence *iface)
{
    return CONTAINING_RECORD(iface, struct d3d11_fence, ID3D11Fence_iface);
}

static const ID3D11FenceVtbl d3d11_fence_vtbl;

/* Fences reaching Signal/Wait may come from other implementations (e.g.
 * dcomp's always-signaled GetAvailableFence object), so type-check before
 * downcasting. */
static struct d3d11_fence *unsafe_impl_from_ID3D11Fence(ID3D11Fence *iface)
{
    if (!iface || iface->lpVtbl != &d3d11_fence_vtbl)
        return NULL;
    return impl_from_ID3D11Fence(iface);
}

static HRESULT STDMETHODCALLTYPE d3d11_fence_QueryInterface(ID3D11Fence *iface, REFIID riid, void **object)
{
    TRACE("iface %p, riid %s, object %p.\n", iface, debugstr_guid(riid), object);

    if (IsEqualGUID(riid, &IID_ID3D11Fence)
            || IsEqualGUID(riid, &IID_ID3D11DeviceChild)
            || IsEqualGUID(riid, &IID_IUnknown))
    {
        ID3D11Fence_AddRef(iface);
        *object = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(riid));

    *object = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE d3d11_fence_AddRef(ID3D11Fence *iface)
{
    struct d3d11_fence *fence = impl_from_ID3D11Fence(iface);
    ULONG refcount = InterlockedIncrement(&fence->refcount);

    TRACE("%p increasing refcount to %lu.\n", fence, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE d3d11_fence_Release(ID3D11Fence *iface)
{
    struct d3d11_fence *fence = impl_from_ID3D11Fence(iface);
    ULONG refcount = InterlockedDecrement(&fence->refcount);

    TRACE("%p decreasing refcount to %lu.\n", fence, refcount);

    if (!refcount)
    {
        /* Waiter events belong to their registrars; don't close them. */
        free(fence->waiters);
        fence->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&fence->cs);
        wined3d_private_store_cleanup(&fence->private_store);
        ID3D11Device5_Release(fence->device);
        free(fence);
    }

    return refcount;
}

static void STDMETHODCALLTYPE d3d11_fence_GetDevice(ID3D11Fence *iface, ID3D11Device **device)
{
    struct d3d11_fence *fence = impl_from_ID3D11Fence(iface);

    TRACE("iface %p, device %p.\n", iface, device);

    *device = (ID3D11Device *)fence->device;
    ID3D11Device_AddRef(*device);
}

static HRESULT STDMETHODCALLTYPE d3d11_fence_GetPrivateData(ID3D11Fence *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct d3d11_fence *fence = impl_from_ID3D11Fence(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_get_private_data(&fence->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_fence_SetPrivateData(ID3D11Fence *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct d3d11_fence *fence = impl_from_ID3D11Fence(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return d3d_set_private_data(&fence->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_fence_SetPrivateDataInterface(ID3D11Fence *iface,
        REFGUID guid, const IUnknown *data)
{
    struct d3d11_fence *fence = impl_from_ID3D11Fence(iface);

    TRACE("iface %p, guid %s, data %p.\n", iface, debugstr_guid(guid), data);

    return d3d_set_private_data_interface(&fence->private_store, guid, data);
}

static HRESULT STDMETHODCALLTYPE d3d11_fence_CreateSharedHandle(ID3D11Fence *iface,
        const SECURITY_ATTRIBUTES *attributes, DWORD access, const WCHAR *name, HANDLE *handle)
{
    struct d3d11_fence *fence = impl_from_ID3D11Fence(iface);
    HANDLE token, caller_handle;

    TRACE("iface %p, attributes %p, access %#lx, name %s, handle %p.\n",
            iface, attributes, access, debugstr_w(name), handle);

    if (!handle)
        return E_INVALIDARG;

    if (name)
    {
        FIXME("Named sharing not supported.\n");
        return E_NOTIMPL;
    }
    if (attributes)
        FIXME("Ignoring security attributes %p.\n", attributes);

    if (!(fence->flags & D3D11_FENCE_FLAG_SHARED))
    {
        WARN("Fence was not created with D3D11_FENCE_FLAG_SHARED.\n");
        return E_INVALIDARG;
    }

    if (!(token = CreateEventW(NULL, TRUE, FALSE, NULL)))
        return E_FAIL;

    if (!DuplicateHandle(GetCurrentProcess(), token, GetCurrentProcess(),
            &caller_handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        CloseHandle(token);
        return E_FAIL;
    }

    EnterCriticalSection(&fence_share_cs);
    if (!fence_array_reserve((void **)&fence_shares, &fence_shares_size,
            fence_share_count + 1, sizeof(*fence_shares)))
    {
        LeaveCriticalSection(&fence_share_cs);
        CloseHandle(caller_handle);
        CloseHandle(token);
        return E_OUTOFMEMORY;
    }
    fence_shares[fence_share_count].token = token;
    fence_shares[fence_share_count].fence = fence;
    ++fence_share_count;
    ID3D11Fence_AddRef(iface);
    LeaveCriticalSection(&fence_share_cs);

    *handle = caller_handle;
    return S_OK;
}

static UINT64 STDMETHODCALLTYPE d3d11_fence_GetCompletedValue(ID3D11Fence *iface)
{
    struct d3d11_fence *fence = impl_from_ID3D11Fence(iface);
    UINT64 completed;

    TRACE("iface %p.\n", iface);

    EnterCriticalSection(&fence->cs);
    completed = fence->completed;
    LeaveCriticalSection(&fence->cs);

    return completed;
}

static HRESULT STDMETHODCALLTYPE d3d11_fence_SetEventOnCompletion(ID3D11Fence *iface,
        UINT64 value, HANDLE event)
{
    struct d3d11_fence *fence = impl_from_ID3D11Fence(iface);

    TRACE("iface %p, value %s, event %p.\n", iface, wine_dbgstr_longlong(value), event);

    EnterCriticalSection(&fence->cs);

    if (value <= fence->completed)
    {
        LeaveCriticalSection(&fence->cs);
        if (event)
            SetEvent(event);
        return S_OK;
    }

    /* D3D12 semantics: a NULL event with a not yet reached value would busy
     * wait until the value is reached; our CPU timeline has nothing to spin
     * on, so simply succeed without registering anything. */
    if (!event)
    {
        LeaveCriticalSection(&fence->cs);
        return S_OK;
    }

    if (!fence_array_reserve((void **)&fence->waiters, &fence->waiters_size,
            fence->waiter_count + 1, sizeof(*fence->waiters)))
    {
        LeaveCriticalSection(&fence->cs);
        return E_OUTOFMEMORY;
    }
    fence->waiters[fence->waiter_count].value = value;
    fence->waiters[fence->waiter_count].event = event;
    ++fence->waiter_count;

    LeaveCriticalSection(&fence->cs);

    return S_OK;
}

static const ID3D11FenceVtbl d3d11_fence_vtbl =
{
    /* IUnknown methods */
    d3d11_fence_QueryInterface,
    d3d11_fence_AddRef,
    d3d11_fence_Release,
    /* ID3D11DeviceChild methods */
    d3d11_fence_GetDevice,
    d3d11_fence_GetPrivateData,
    d3d11_fence_SetPrivateData,
    d3d11_fence_SetPrivateDataInterface,
    /* ID3D11Fence methods */
    d3d11_fence_CreateSharedHandle,
    d3d11_fence_GetCompletedValue,
    d3d11_fence_SetEventOnCompletion,
};

HRESULT d3d11_fence_create(ID3D11Device5 *device, UINT64 initial_value,
        D3D11_FENCE_FLAG flags, REFIID iid, void **fence)
{
    struct d3d11_fence *object;
    HRESULT hr;

    /* Wine's d3d11_3.idl defines D3D11_FENCE_FLAG_NONE as 0x1 (the platform
     * SDK uses 0), so accept both spellings of "no flags".  Anything else
     * (SHARED_CROSS_ADAPTER, NON_MONITORED) is rejected. */
    if (flags & ~(D3D11_FENCE_FLAG_NONE | D3D11_FENCE_FLAG_SHARED))
    {
        FIXME("Unsupported flags %#x.\n", flags);
        return E_INVALIDARG;
    }

    if (!(object = calloc(1, sizeof(*object))))
        return E_OUTOFMEMORY;

    object->ID3D11Fence_iface.lpVtbl = &d3d11_fence_vtbl;
    object->refcount = 1;
    object->flags = flags;
    object->completed = initial_value;
    wined3d_private_store_init(&object->private_store);
    InitializeCriticalSectionEx(&object->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    object->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": d3d11_fence.cs");
    object->device = device;
    ID3D11Device5_AddRef(device);

    hr = ID3D11Fence_QueryInterface(&object->ID3D11Fence_iface, iid, fence);
    ID3D11Fence_Release(&object->ID3D11Fence_iface);
    return hr;
}

HRESULT d3d11_fence_open_shared(HANDLE handle, REFIID iid, void **fence)
{
    struct d3d11_fence *object = NULL;
    HRESULT hr;
    SIZE_T i;

    EnterCriticalSection(&fence_share_cs);
    for (i = 0; i < fence_share_count; ++i)
    {
        if (!NtCompareObjects(fence_shares[i].token, handle))
        {
            object = fence_shares[i].fence;
            break;
        }
    }
    if (object)
        hr = ID3D11Fence_QueryInterface(&object->ID3D11Fence_iface, iid, fence);
    LeaveCriticalSection(&fence_share_cs);

    if (!object)
    {
        FIXME("Unknown handle %p; cross-process fence sharing is not supported.\n", handle);
        return E_INVALIDARG;
    }

    return hr;
}

/* Advance the timeline and fire due waiters.  Called from
 * ID3D11DeviceContext4::Signal after the wined3d flush. */
HRESULT d3d11_fence_signal(ID3D11Fence *iface, UINT64 value)
{
    struct d3d11_fence *fence = unsafe_impl_from_ID3D11Fence(iface);
    SIZE_T i;

    if (!fence)
    {
        FIXME("Fence %p was not created by d3d11.\n", iface);
        return E_INVALIDARG;
    }

    EnterCriticalSection(&fence->cs);

    if (value > fence->completed)
        fence->completed = value;

    for (i = 0; i < fence->waiter_count;)
    {
        if (fence->waiters[i].value <= fence->completed)
        {
            SetEvent(fence->waiters[i].event);
            fence->waiters[i] = fence->waiters[--fence->waiter_count];
        }
        else
        {
            ++i;
        }
    }

    LeaveCriticalSection(&fence->cs);

    return S_OK;
}

static void d3d11_fence_unregister_waiter(struct d3d11_fence *fence, UINT64 value, HANDLE event)
{
    SIZE_T i;

    EnterCriticalSection(&fence->cs);
    for (i = 0; i < fence->waiter_count; ++i)
    {
        if (fence->waiters[i].event == event && fence->waiters[i].value == value)
        {
            fence->waiters[i] = fence->waiters[--fence->waiter_count];
            break;
        }
    }
    LeaveCriticalSection(&fence->cs);
}

/* Wait through the public ID3D11Fence interface so that foreign
 * implementations (e.g. dcomp's always-signaled fence) work too.  The wait
 * is bounded: our composite pipeline is CPU-synchronous, so a signal that
 * has not arrived within the timeout is either produced on another thread
 * (tolerable race) or will never come; blocking indefinitely here would
 * risk deadlocking the caller's render loop. */
HRESULT d3d11_fence_wait(ID3D11Fence *iface, UINT64 value)
{
    struct d3d11_fence *fence = unsafe_impl_from_ID3D11Fence(iface);
    HANDLE event;
    HRESULT hr;

    if (ID3D11Fence_GetCompletedValue(iface) >= value)
        return S_OK;

    if (!(event = CreateEventW(NULL, TRUE, FALSE, NULL)))
        return E_FAIL;

    if (SUCCEEDED(hr = ID3D11Fence_SetEventOnCompletion(iface, value, event)))
    {
        if (WaitForSingleObject(event, 100) == WAIT_TIMEOUT)
            WARN("Timed out waiting for fence %p, value %s, completed %s.\n", iface,
                    wine_dbgstr_longlong(value),
                    wine_dbgstr_longlong(ID3D11Fence_GetCompletedValue(iface)));
    }

    /* Drop the registration (if still pending) before the event handle
     * becomes invalid. */
    if (fence)
        d3d11_fence_unregister_waiter(fence, value, event);
    CloseHandle(event);

    return S_OK;
}
