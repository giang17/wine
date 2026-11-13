/*
 * Unit tests for DirectComposition
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

#define COBJMACROS
#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "initguid.h"
#include "d3d11.h"
#include "dxgi1_2.h"
#include "dcomp.h"
#include "wine/dcomp_layer.h"
#include "wine/test.h"

/* Not declared in dcomp.h. */
HRESULT WINAPI DCompositionCreateDevice3(IUnknown *rendering_device, REFIID iid, void **device);

static const char test_class[] = "dcomp_test_window";

static LONG get_refcount(void *iface)
{
    IUnknown *unk = iface;
    IUnknown_AddRef(unk);
    return IUnknown_Release(unk);
}

static void flush_events(DWORD timeout)
{
    DWORD end = GetTickCount() + timeout, now;
    MSG msg;

    for (;;)
    {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
            DispatchMessageA(&msg);
        now = GetTickCount();
        if ((int)(end - now) <= 0)
            break;
        MsgWaitForMultipleObjects(0, NULL, FALSE, end - now, QS_ALLINPUT);
    }
}

static HWND create_window(void)
{
    HWND hwnd = CreateWindowA(test_class, "dcomp test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            20, 20, 320, 240, NULL, NULL, GetModuleHandleA(NULL), NULL);
    ok(!!hwnd, "Failed to create a window, error %lu.\n", GetLastError());
    flush_events(50);
    return hwnd;
}

static ID3D11Device *create_d3d11_device(void)
{
    static const D3D_DRIVER_TYPE types[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
    ID3D11Device *device;
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(types); ++i)
    {
        if (SUCCEEDED(D3D11CreateDevice(NULL, types[i], NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                NULL, 0, D3D11_SDK_VERSION, &device, NULL, NULL)))
            return device;
    }
    return NULL;
}

static IDXGISwapChain1 *create_composition_swapchain(ID3D11Device *device, unsigned int width, unsigned int height)
{
    IDXGISwapChain1 *swapchain = NULL;
    DXGI_SWAP_CHAIN_DESC1 desc = {0};
    IDXGIDevice *dxgi_device;
    IDXGIFactory2 *factory;
    IDXGIAdapter *adapter;
    HRESULT hr;

    hr = ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi_device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDXGIDevice_GetAdapter(dxgi_device, &adapter);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    hr = IDXGIFactory2_CreateSwapChainForComposition(factory, (IUnknown *)device, &desc, NULL, &swapchain);
    ok(hr == S_OK, "CreateSwapChainForComposition failed, hr %#lx.\n", hr);

    IDXGIFactory2_Release(factory);
    IDXGIAdapter_Release(adapter);
    IDXGIDevice_Release(dxgi_device);
    return swapchain;
}

static void test_create_device(void)
{
    /* Which interface each creation function hands out and which ones the
     * object answers to.  IDCompositionDevice4 and 5 depend on the Windows
     * version and are left out. */
    static const struct
    {
        const GUID *iid;
        BOOL create[3];
        BOOL query[3];
    }
    iids[] =
    {
        {&IID_IUnknown,                   {FALSE, FALSE, FALSE}, {TRUE,  TRUE,  TRUE}},
        {&IID_IDCompositionDevice,        {TRUE,  TRUE,  TRUE},  {TRUE,  TRUE,  TRUE}},
        {&IID_IDCompositionDevice2,       {FALSE, FALSE, FALSE}, {FALSE, TRUE,  TRUE}},
        {&IID_IDCompositionDesktopDevice, {FALSE, TRUE,  TRUE},  {FALSE, TRUE,  TRUE}},
        {&IID_IDCompositionDevice3,       {FALSE, FALSE, FALSE}, {FALSE, FALSE, TRUE}},
        {&IID_IDXGISurface,               {FALSE, FALSE, FALSE}, {FALSE, FALSE, FALSE}},
    };
    IDXGIDevice *dxgi_device = NULL;
    ID3D11Device *d3d_device;
    unsigned int i, j, k, l;
    IUnknown *unk, *unk2;
    HRESULT hr;

    if ((d3d_device = create_d3d11_device()))
    {
        hr = ID3D11Device_QueryInterface(d3d_device, &IID_IDXGIDevice, (void **)&dxgi_device);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
    }
    else
        skip("Failed to create a D3D11 device.\n");

    /* i: creation function, j: without and with a rendering device, k: requested interface */
    for (i = 0; i < 3; ++i)
    {
        for (j = 0; j < (dxgi_device ? 2 : 1); ++j)
        {
            for (k = 0; k < ARRAY_SIZE(iids); ++k)
            {
                winetest_push_context("function %u, device %u, iid %u", i, j, k);
                unk = (IUnknown *)0xdeadbeef;
                if (i == 0)
                    hr = DCompositionCreateDevice(j ? dxgi_device : NULL, iids[k].iid, (void **)&unk);
                else if (i == 1)
                    hr = DCompositionCreateDevice2(j ? (IUnknown *)dxgi_device : NULL, iids[k].iid, (void **)&unk);
                else
                    hr = DCompositionCreateDevice3(j ? (IUnknown *)dxgi_device : NULL, iids[k].iid, (void **)&unk);
                todo_wine_if(!iids[k].create[i] && iids[k].iid != &IID_IDXGISurface)
                ok(hr == (iids[k].create[i] ? S_OK : E_NOINTERFACE), "Got hr %#lx.\n", hr);
                if (FAILED(hr))
                {
                    ok(!unk, "Got object %p.\n", unk);
                    winetest_pop_context();
                    continue;
                }

                for (l = 0; l < ARRAY_SIZE(iids); ++l)
                {
                    hr = IUnknown_QueryInterface(unk, iids[l].iid, (void **)&unk2);
                    todo_wine_if(!iids[l].query[i] && iids[l].iid != &IID_IDXGISurface)
                    ok(hr == (iids[l].query[i] ? S_OK : E_NOINTERFACE), "QueryInterface %u: got hr %#lx.\n", l, hr);
                    if (SUCCEEDED(hr))
                        IUnknown_Release(unk2);
                }
                IUnknown_Release(unk);
                winetest_pop_context();
            }
        }
    }

    hr = DCompositionCreateDevice(NULL, &IID_IDCompositionDevice, NULL);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = DCompositionCreateDevice2(NULL, &IID_IDCompositionDesktopDevice, NULL);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = DCompositionCreateDevice3(NULL, &IID_IDCompositionDesktopDevice, NULL);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);

    if (dxgi_device)
        IDXGIDevice_Release(dxgi_device);
    if (d3d_device)
        ID3D11Device_Release(d3d_device);
}

static void test_visual(void)
{
    static const D2D_MATRIX_3X2_F identity = {{{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}}};
    static const D2D_RECT_F clip_rect = {0.0f, 0.0f, 10.0f, 10.0f};
    IDCompositionVisual2 *parent, *child1, *child2, *child3, *stranger;
    IDCompositionDesktopDevice *device;
    IDCompositionVisual3 *visual3;
    D2D_MATRIX_4X4_F identity4;
    IDCompositionVisual *visual;
    IUnknown *unk;
    LONG refcount;
    HRESULT hr;

    hr = DCompositionCreateDevice2(NULL, &IID_IDCompositionDesktopDevice, (void **)&device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (FAILED(hr))
        return;

    hr = IDCompositionDesktopDevice_CreateVisual(device, NULL);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateVisual(device, &parent);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    refcount = get_refcount(parent);
    ok(refcount == 1, "Got refcount %ld.\n", refcount);

    hr = IDCompositionVisual2_QueryInterface(parent, &IID_IDCompositionVisual, (void **)&visual);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    IDCompositionVisual_Release(visual);
    hr = IDCompositionVisual2_QueryInterface(parent, &IID_IDCompositionVisualDebug, (void **)&unk);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    IUnknown_Release(unk);
    hr = IDCompositionVisual2_QueryInterface(parent, &IID_IDCompositionDevice, (void **)&unk);
    ok(hr == E_NOINTERFACE, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_QueryInterface(parent, &IID_IDCompositionVisual3, (void **)&visual3);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    /* Property setters. */
    hr = IDCompositionVisual2_SetOffsetX(parent, 10.0f);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetOffsetY(parent, 20.0f);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetOffsetXAnimation(parent, NULL);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetTransform(parent, &identity);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetTransformObject(parent, NULL);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetTransformParent(parent, NULL);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetEffect(parent, NULL);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetBitmapInterpolationMode(parent, DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetBitmapInterpolationMode(parent, 0x1234);
    todo_wine ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetBorderMode(parent, DCOMPOSITION_BORDER_MODE_HARD);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetBorderMode(parent, 0x1234);
    todo_wine ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetClipObject(parent, NULL);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetClip(parent, &clip_rect);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetContent(parent, NULL);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    /* An object that is neither a surface nor a swapchain. */
    hr = IDCompositionVisual2_SetContent(parent, (IUnknown *)visual3);
    todo_wine ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetContent(parent, NULL);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetCompositeMode(parent, DCOMPOSITION_COMPOSITE_MODE_SOURCE_OVER);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetCompositeMode(parent, 0x1234);
    todo_wine ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetOpacityMode(parent, DCOMPOSITION_OPACITY_MODE_MULTIPLY);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetBackFaceVisibility(parent, DCOMPOSITION_BACKFACE_VISIBILITY_HIDDEN);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    memset(&identity4, 0, sizeof(identity4));
    identity4._11 = identity4._22 = identity4._33 = identity4._44 = 1.0f;
    hr = IDCompositionVisual3_SetDepthMode(visual3, DCOMPOSITION_DEPTH_MODE_TREE);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual3_SetOffsetZ(visual3, 1.0f);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual3_SetOpacity(visual3, 0.5f);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual3_SetOpacity(visual3, -1.0f);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual3_SetOpacity(visual3, 2.0f);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual3_SetTransform3D(visual3, &identity4);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual3_SetTransform3DObject(visual3, NULL);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual3_SetVisible(visual3, FALSE);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual3_SetVisible(visual3, TRUE);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    IDCompositionVisual3_Release(visual3);

    /* The tree. */
    hr = IDCompositionDesktopDevice_CreateVisual(device, &child1);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateVisual(device, &child2);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateVisual(device, &child3);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateVisual(device, &stranger);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    hr = IDCompositionVisual2_AddVisual(parent, NULL, TRUE, NULL);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_AddVisual(parent, (IDCompositionVisual *)child1, TRUE, NULL);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    refcount = get_refcount(child1);
    todo_wine ok(refcount == 1, "Got refcount %ld.\n", refcount);
    refcount = get_refcount(parent);
    ok(refcount == 1, "Got refcount %ld.\n", refcount);
    /* A visual that already has a parent, this one or another. */
    hr = IDCompositionVisual2_AddVisual(parent, (IDCompositionVisual *)child1, TRUE, NULL);
    todo_wine ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_AddVisual(stranger, (IDCompositionVisual *)child1, TRUE, NULL);
    todo_wine ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    if (hr == S_OK) /* Wine moved it, put it back. */
        IDCompositionVisual2_AddVisual(parent, (IDCompositionVisual *)child1, TRUE, NULL);
    /* The visual itself. */
    hr = IDCompositionVisual2_AddVisual(parent, (IDCompositionVisual *)parent, TRUE, NULL);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    /* Relative to a child, and relative to a visual that is not a child. */
    hr = IDCompositionVisual2_AddVisual(parent, (IDCompositionVisual *)child2, TRUE, (IDCompositionVisual *)child1);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_AddVisual(parent, (IDCompositionVisual *)child2, FALSE, (IDCompositionVisual *)child1);
    todo_wine ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_AddVisual(parent, (IDCompositionVisual *)child3, TRUE, (IDCompositionVisual *)stranger);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);

    hr = IDCompositionVisual2_RemoveVisual(parent, NULL);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_RemoveVisual(parent, (IDCompositionVisual *)stranger);
    todo_wine ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_RemoveVisual(parent, (IDCompositionVisual *)child1);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    refcount = get_refcount(child1);
    ok(refcount == 1, "Got refcount %ld.\n", refcount);
    hr = IDCompositionVisual2_RemoveVisual(parent, (IDCompositionVisual *)child1);
    todo_wine ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_RemoveAllVisuals(parent);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    refcount = get_refcount(child2);
    ok(refcount == 1, "Got refcount %ld.\n", refcount);
    hr = IDCompositionVisual2_RemoveAllVisuals(parent);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    hr = IDCompositionDesktopDevice_Commit(device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_WaitForCommitCompletion(device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    IDCompositionVisual2_Release(stranger);
    IDCompositionVisual2_Release(child3);
    IDCompositionVisual2_Release(child2);
    IDCompositionVisual2_Release(child1);
    IDCompositionVisual2_Release(parent);
    refcount = IDCompositionDesktopDevice_Release(device);
    todo_wine ok(!refcount, "Got refcount %ld.\n", refcount);
}

static void test_target(void)
{
    IDCompositionTarget *target, *target2;
    IDCompositionDesktopDevice *device;
    IDCompositionVisual2 *visual;
    IDCompositionDevice *device1;
    HWND hwnd, hwnd2;
    LONG refcount;
    HRESULT hr;

    hr = DCompositionCreateDevice2(NULL, &IID_IDCompositionDesktopDevice, (void **)&device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (FAILED(hr))
        return;
    hr = IDCompositionDesktopDevice_QueryInterface(device, &IID_IDCompositionDevice, (void **)&device1);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    hwnd = create_window();
    hwnd2 = create_window();

    hr = IDCompositionDesktopDevice_CreateTargetForHwnd(device, NULL, TRUE, &target);
    todo_wine ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    if (SUCCEEDED(hr)) IDCompositionTarget_Release(target);
    hr = IDCompositionDesktopDevice_CreateTargetForHwnd(device, (HWND)0xdeadbeef, TRUE, &target);
    todo_wine ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    if (SUCCEEDED(hr)) IDCompositionTarget_Release(target);
    hr = IDCompositionDesktopDevice_CreateTargetForHwnd(device, hwnd, TRUE, NULL);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);

    hr = IDCompositionDesktopDevice_CreateTargetForHwnd(device, hwnd, TRUE, &target);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    refcount = get_refcount(target);
    ok(refcount == 1, "Got refcount %ld.\n", refcount);

    /* One target per window and layer. */
    hr = IDCompositionDesktopDevice_CreateTargetForHwnd(device, hwnd, TRUE, &target2);
    todo_wine ok(hr == DCOMPOSITION_ERROR_WINDOW_ALREADY_COMPOSED, "Got hr %#lx.\n", hr);
    if (SUCCEEDED(hr)) IDCompositionTarget_Release(target2);
    hr = IDCompositionDesktopDevice_CreateTargetForHwnd(device, hwnd, FALSE, &target2);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (SUCCEEDED(hr)) IDCompositionTarget_Release(target2);
    hr = IDCompositionDevice_CreateTargetForHwnd(device1, hwnd2, TRUE, &target2);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (SUCCEEDED(hr)) IDCompositionTarget_Release(target2);

    hr = IDCompositionDesktopDevice_CreateVisual(device, &visual);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionTarget_SetRoot(target, NULL);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionTarget_SetRoot(target, (IDCompositionVisual *)visual);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    refcount = get_refcount(visual);
    todo_wine ok(refcount == 1, "Got refcount %ld.\n", refcount);
    hr = IDCompositionTarget_SetRoot(target, (IDCompositionVisual *)visual);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_Commit(device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    flush_events(50);
    hr = IDCompositionTarget_SetRoot(target, NULL);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    refcount = get_refcount(visual);
    ok(refcount == 1, "Got refcount %ld.\n", refcount);
    hr = IDCompositionDesktopDevice_Commit(device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    /* After the target is gone the window takes a new one. */
    IDCompositionTarget_Release(target);
    hr = IDCompositionDesktopDevice_CreateTargetForHwnd(device, hwnd, TRUE, &target2);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (SUCCEEDED(hr)) IDCompositionTarget_Release(target2);

    IDCompositionVisual2_Release(visual);
    IDCompositionDevice_Release(device1);
    IDCompositionDesktopDevice_Release(device);
    DestroyWindow(hwnd2);
    DestroyWindow(hwnd);
}

static void test_surface(void)
{
    IDCompositionVirtualSurface *virtual_surface;
    IDCompositionDesktopDevice *device;
    IDCompositionSurface *surface;
    IDCompositionVisual2 *visual;
    IDXGIDevice *dxgi_device;
    ID3D11Device *d3d_device;
    RECT rect = {8, 8, 24, 24};
    IUnknown *unk, *unk2;
    LONG refcount;
    POINT offset;
    HRESULT hr;

    if (!(d3d_device = create_d3d11_device()))
    {
        skip("Failed to create a D3D11 device.\n");
        return;
    }
    hr = ID3D11Device_QueryInterface(d3d_device, &IID_IDXGIDevice, (void **)&dxgi_device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    /* A device without a rendering device has no surfaces. */
    hr = DCompositionCreateDevice2(NULL, &IID_IDCompositionDesktopDevice, (void **)&device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateSurface(device, 64, 64, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
    todo_wine ok(hr == E_ILLEGAL_METHOD_CALL, "Got hr %#lx.\n", hr);
    if (SUCCEEDED(hr)) IDCompositionSurface_Release(surface);
    IDCompositionDesktopDevice_Release(device);

    hr = DCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDesktopDevice, (void **)&device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    hr = IDCompositionDesktopDevice_CreateSurface(device, 64, 64, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_PREMULTIPLIED, NULL);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateSurface(device, 0, 0, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateSurface(device, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (SUCCEEDED(hr)) IDCompositionSurface_Release(surface);
    hr = IDCompositionDesktopDevice_CreateSurface(device, 64, 64, DXGI_FORMAT_NV12,
            DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
    todo_wine ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    if (SUCCEEDED(hr)) IDCompositionSurface_Release(surface);
    hr = IDCompositionDesktopDevice_CreateSurface(device, 64, 64, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_STRAIGHT, &surface);
    todo_wine ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    if (SUCCEEDED(hr)) IDCompositionSurface_Release(surface);
    hr = IDCompositionDesktopDevice_CreateSurface(device, 64, 64, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_IGNORE, &surface);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (SUCCEEDED(hr)) IDCompositionSurface_Release(surface);

    hr = IDCompositionDesktopDevice_CreateSurface(device, 64, 64, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (FAILED(hr))
        goto done;

    /* Outside of a BeginDraw()/EndDraw() pair. */
    hr = IDCompositionSurface_EndDraw(surface);
    todo_wine ok(hr == DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED, "Got hr %#lx.\n", hr);
    hr = IDCompositionSurface_SuspendDraw(surface);
    todo_wine ok(hr == DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED, "Got hr %#lx.\n", hr);
    hr = IDCompositionSurface_ResumeDraw(surface);
    todo_wine ok(hr == DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED, "Got hr %#lx.\n", hr);

    hr = IDCompositionSurface_BeginDraw(surface, NULL, &IID_IDCompositionDevice, (void **)&unk, &offset);
    ok(hr == E_NOINTERFACE, "Got hr %#lx.\n", hr);
    hr = IDCompositionSurface_BeginDraw(surface, NULL, &IID_IDXGISurface, NULL, &offset);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);

    hr = IDCompositionSurface_BeginDraw(surface, NULL, &IID_IDXGISurface, (void **)&unk, &offset);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    /* A second BeginDraw() succeeds while the first is open; one EndDraw() closes both. */
    hr = IDCompositionSurface_BeginDraw(surface, NULL, &IID_IDXGISurface, (void **)&unk2, &offset);
    todo_wine ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (SUCCEEDED(hr)) IUnknown_Release(unk2);
    hr = IDCompositionSurface_SuspendDraw(surface);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionSurface_ResumeDraw(surface);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    IUnknown_Release(unk);
    hr = IDCompositionSurface_EndDraw(surface);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionSurface_EndDraw(surface);
    todo_wine ok(hr == DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED, "Got hr %#lx.\n", hr);

    /* An update rectangle inside the surface, and one that reaches outside. */
    hr = IDCompositionSurface_BeginDraw(surface, &rect, &IID_IDXGISurface, (void **)&unk, &offset);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    IUnknown_Release(unk);
    hr = IDCompositionSurface_EndDraw(surface);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    SetRect(&rect, 32, 32, 128, 128);
    hr = IDCompositionSurface_BeginDraw(surface, &rect, &IID_IDXGISurface, (void **)&unk, &offset);
    todo_wine ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        IUnknown_Release(unk);
        IDCompositionSurface_EndDraw(surface);
    }
    SetRect(&rect, 0, 0, 16, 16);
    hr = IDCompositionSurface_Scroll(surface, &rect, NULL, 4, 4);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionSurface_Scroll(surface, NULL, NULL, 0, 0);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    hr = IDCompositionDesktopDevice_CreateVisual(device, &visual);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetContent(visual, (IUnknown *)surface);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    refcount = get_refcount(surface);
    todo_wine ok(refcount == 1, "Got refcount %ld.\n", refcount);
    hr = IDCompositionVisual2_SetContent(visual, NULL);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    refcount = get_refcount(surface);
    ok(refcount == 1, "Got refcount %ld.\n", refcount);
    IDCompositionVisual2_Release(visual);
    IDCompositionSurface_Release(surface);

    hr = IDCompositionDesktopDevice_CreateVirtualSurface(device, 4096, 4096, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_PREMULTIPLIED, &virtual_surface);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = IDCompositionVirtualSurface_Resize(virtual_surface, 2048, 2048);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        hr = IDCompositionVirtualSurface_Resize(virtual_surface, 0, 0);
        todo_wine ok(hr == S_OK, "Got hr %#lx.\n", hr);
        hr = IDCompositionVirtualSurface_Trim(virtual_surface, NULL, 0);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        hr = IDCompositionVirtualSurface_Trim(virtual_surface, &rect, 1);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        IDCompositionVirtualSurface_Release(virtual_surface);
    }

done:
    IDCompositionDesktopDevice_Release(device);
    IDXGIDevice_Release(dxgi_device);
    ID3D11Device_Release(d3d_device);
}

static void test_object_creation(void)
{
    IDCompositionDevice *device;
    IUnknown *unk;
    HRESULT hr;
    BOOL valid;

    hr = DCompositionCreateDevice(NULL, &IID_IDCompositionDevice, (void **)&device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    if (FAILED(hr))
        return;

    /* Transforms and clips are not implemented in Wine.  The markers report
     * "succeeded inside todo block" as soon as one of them is. */
#define check_create(method, todo) do { unk = NULL; hr = IDCompositionDevice_##method(device, (void *)&unk); \
        todo_wine_if(todo) ok(hr == S_OK && unk, "%s: got hr %#lx, object %p.\n", #method, hr, unk); \
        if (SUCCEEDED(hr) && unk) IUnknown_Release(unk); } while (0)
    check_create(CreateTranslateTransform, TRUE);
    check_create(CreateScaleTransform, TRUE);
    check_create(CreateRotateTransform, TRUE);
    check_create(CreateSkewTransform, TRUE);
    check_create(CreateMatrixTransform, TRUE);
    check_create(CreateTranslateTransform3D, TRUE);
    check_create(CreateScaleTransform3D, TRUE);
    check_create(CreateRotateTransform3D, TRUE);
    check_create(CreateMatrixTransform3D, TRUE);
    check_create(CreateRectangleClip, TRUE);
    check_create(CreateEffectGroup, FALSE);
    check_create(CreateAnimation, FALSE);
#undef check_create

    hr = IDCompositionDevice_CreateSurfaceFromHwnd(device, NULL, &unk);
    todo_wine ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);

    valid = 0xdead;
    hr = IDCompositionDevice_CheckDeviceState(device, &valid);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    ok(valid == TRUE, "Got valid %d.\n", valid);
    hr = IDCompositionDevice_GetFrameStatistics(device, NULL);
    ok(hr == E_INVALIDARG, "Got hr %#lx.\n", hr);
    hr = IDCompositionDevice_Commit(device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionDevice_WaitForCommitCompletion(device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    IDCompositionDevice_Release(device);
}

/* Wine only: the leaves of a visual tree as dcomp hands them to wined3d, in
 * properties of the target window.  Only composition swapchain leaves are
 * written, depth first, with the offsets and opacities of their ancestors
 * accumulated. */
static ULONG_PTR get_child_prop(HWND hwnd, unsigned int idx, const WCHAR *format)
{
    WCHAR buffer[64];

    swprintf(buffer, ARRAY_SIZE(buffer), format, idx);
    return (ULONG_PTR)GetPropW(hwnd, buffer);
}

#define check_leaf(hwnd, idx, x, y, opacity) check_leaf_(__LINE__, hwnd, idx, x, y, opacity)
static void check_leaf_(unsigned int line, HWND hwnd, unsigned int idx, int x, int y, float opacity)
{
    ULONG_PTR offset = get_child_prop(hwnd, idx, WINE_DCOMP_CHILD_OFFSET_PROP_FMT);
    ULONG_PTR prop = get_child_prop(hwnd, idx, WINE_DCOMP_CHILD_OPACITY_PROP_FMT);
    HWND wnd = (HWND)get_child_prop(hwnd, idx, WINE_DCOMP_CHILD_WND_PROP_FMT);

    ok_(__FILE__, line)(wnd && IsWindow(wnd), "Leaf %u: got composition window %p.\n", idx, wnd);
    ok_(__FILE__, line)((short)LOWORD(offset) == (short)x && (short)HIWORD(offset) == (short)y,
            "Leaf %u: got offset (%d,%d), expected (%d,%d).\n", idx, (short)LOWORD(offset),
            (short)HIWORD(offset), (short)x, (short)y);
    ok_(__FILE__, line)(prop == wine_dcomp_opacity_to_prop(opacity), "Leaf %u: got opacity %Iu, expected %u.\n",
            idx, prop, wine_dcomp_opacity_to_prop(opacity));
}

static void test_leaf_serialization(void)
{
    IDCompositionVisual2 *root, *group, *leaf[18], *surface_visual;
    IDCompositionVisual3 *group3, *leaf3;
    IDCompositionDesktopDevice *device;
    IDXGISwapChain1 *swapchain[18];
    IDCompositionSurface *surface;
    IDCompositionTarget *target;
    ULONG_PTR count, gen, gen2;
    IDXGIDevice *dxgi_device;
    ID3D11Device *d3d_device;
    unsigned int i;
    HRESULT hr;
    HWND hwnd;

    if (!winetest_platform_is_wine)
    {
        skip("The leaf properties are internal to Wine.\n");
        return;
    }
    if (!(d3d_device = create_d3d11_device()))
    {
        skip("Failed to create a D3D11 device.\n");
        return;
    }
    hr = ID3D11Device_QueryInterface(d3d_device, &IID_IDXGIDevice, (void **)&dxgi_device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = DCompositionCreateDevice2((IUnknown *)dxgi_device, &IID_IDCompositionDesktopDevice, (void **)&device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    hwnd = create_window();
    hr = IDCompositionDesktopDevice_CreateTargetForHwnd(device, hwnd, TRUE, &target);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    for (i = 0; i < ARRAY_SIZE(leaf); ++i)
    {
        swapchain[i] = create_composition_swapchain(d3d_device, 16, 16);
        hr = IDCompositionDesktopDevice_CreateVisual(device, &leaf[i]);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
        hr = IDCompositionVisual2_SetContent(leaf[i], (IUnknown *)swapchain[i]);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
    }
    hr = IDCompositionDesktopDevice_CreateVisual(device, &root);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateVisual(device, &group);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_QueryInterface(group, &IID_IDCompositionVisual3, (void **)&group3);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    /* A root without children. */
    hr = IDCompositionTarget_SetRoot(target, (IDCompositionVisual *)root);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_Commit(device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    flush_events(100);
    count = (ULONG_PTR)GetPropW(hwnd, WINE_DCOMP_CHILD_COUNT_PROP);
    ok(!count, "Got %Iu leaves.\n", count);
    gen = (ULONG_PTR)GetPropW(hwnd, WINE_DCOMP_CHILD_GEN_PROP);

    /* root (10,20)
     *   leaf 0 (1,2)
     *   group (5,6), opacity 0.5
     *     leaf 1 (7,8)
     *     leaf 2 (0,0), opacity 0.5
     *   leaf 3 (-3,-4) */
    IDCompositionVisual2_SetOffsetX(root, 10.0f);
    IDCompositionVisual2_SetOffsetY(root, 20.0f);
    IDCompositionVisual2_SetOffsetX(leaf[0], 1.0f);
    IDCompositionVisual2_SetOffsetY(leaf[0], 2.0f);
    IDCompositionVisual2_SetOffsetX(group, 5.0f);
    IDCompositionVisual2_SetOffsetY(group, 6.0f);
    IDCompositionVisual3_SetOpacity(group3, 0.5f);
    IDCompositionVisual2_SetOffsetX(leaf[1], 7.0f);
    IDCompositionVisual2_SetOffsetY(leaf[1], 8.0f);
    hr = IDCompositionVisual2_QueryInterface(leaf[2], &IID_IDCompositionVisual3, (void **)&leaf3);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    IDCompositionVisual3_SetOpacity(leaf3, 0.5f);
    IDCompositionVisual3_Release(leaf3);
    IDCompositionVisual2_SetOffsetX(leaf[3], -3.0f);
    IDCompositionVisual2_SetOffsetY(leaf[3], -4.0f);

    hr = IDCompositionVisual2_AddVisual(root, (IDCompositionVisual *)leaf[0], TRUE, NULL);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_AddVisual(root, (IDCompositionVisual *)group, TRUE, NULL);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_AddVisual(group, (IDCompositionVisual *)leaf[1], TRUE, NULL);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_AddVisual(group, (IDCompositionVisual *)leaf[2], TRUE, NULL);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_AddVisual(root, (IDCompositionVisual *)leaf[3], TRUE, NULL);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    hr = IDCompositionDesktopDevice_Commit(device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    flush_events(100);

    count = (ULONG_PTR)GetPropW(hwnd, WINE_DCOMP_CHILD_COUNT_PROP);
    ok(count == 4, "Got %Iu leaves.\n", count);
    gen2 = (ULONG_PTR)GetPropW(hwnd, WINE_DCOMP_CHILD_GEN_PROP);
    ok(!(gen2 & 1) && gen2 > gen, "Got generation %Iu after %Iu.\n", gen2, gen);
    check_leaf(hwnd, 0, 11, 22, 1.0f);
    check_leaf(hwnd, 1, 22, 34, 0.5f);
    check_leaf(hwnd, 2, 15, 26, 0.25f);
    check_leaf(hwnd, 3, 7, 16, 1.0f);
    ok(get_child_prop(hwnd, 0, WINE_DCOMP_CHILD_WND_PROP_FMT) != get_child_prop(hwnd, 1, WINE_DCOMP_CHILD_WND_PROP_FMT), "Leaves 0 and 1 share a window.\n");

    /* A hidden group takes its leaves with it. */
    hr = IDCompositionVisual3_SetVisible(group3, FALSE);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_Commit(device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    flush_events(100);
    count = (ULONG_PTR)GetPropW(hwnd, WINE_DCOMP_CHILD_COUNT_PROP);
    ok(count == 2, "Got %Iu leaves.\n", count);
    check_leaf(hwnd, 0, 11, 22, 1.0f);
    check_leaf(hwnd, 1, 7, 16, 1.0f);
    hr = IDCompositionVisual3_SetVisible(group3, TRUE);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    /* An offset beyond 16 bits wraps in the packed property. */
    IDCompositionVisual2_SetOffsetX(leaf[3], 40000.0f);
    hr = IDCompositionDesktopDevice_Commit(device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    flush_events(100);
    count = (ULONG_PTR)GetPropW(hwnd, WINE_DCOMP_CHILD_COUNT_PROP);
    ok(count == 4, "Got %Iu leaves.\n", count);
    check_leaf(hwnd, 3, 40010, 16, 1.0f);
    IDCompositionVisual2_SetOffsetX(leaf[3], -3.0f);

    /* A surface leaf is not written. */
    hr = IDCompositionDesktopDevice_CreateSurface(device, 16, 16, DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateVisual(device, &surface_visual);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetContent(surface_visual, (IUnknown *)surface);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionVisual2_AddVisual(root, (IDCompositionVisual *)surface_visual, FALSE, NULL);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_Commit(device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    flush_events(100);
    count = (ULONG_PTR)GetPropW(hwnd, WINE_DCOMP_CHILD_COUNT_PROP);
    ok(count == 4, "Got %Iu leaves.\n", count);
    check_leaf(hwnd, 0, 11, 22, 1.0f);
    hr = IDCompositionVisual2_RemoveVisual(root, (IDCompositionVisual *)surface_visual);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);

    /* No more than 16 leaves are written. */
    for (i = 4; i < ARRAY_SIZE(leaf); ++i)
    {
        IDCompositionVisual2_SetOffsetX(leaf[i], (float)i);
        hr = IDCompositionVisual2_AddVisual(root, (IDCompositionVisual *)leaf[i], TRUE, NULL);
        ok(hr == S_OK, "Got hr %#lx.\n", hr);
    }
    hr = IDCompositionDesktopDevice_Commit(device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    flush_events(100);
    count = (ULONG_PTR)GetPropW(hwnd, WINE_DCOMP_CHILD_COUNT_PROP);
    ok(count == 16, "Got %Iu leaves.\n", count);
    check_leaf(hwnd, 15, 10 + 15, 20, 1.0f);
    ok(!get_child_prop(hwnd, 16, WINE_DCOMP_CHILD_WND_PROP_FMT), "Leaf 16 was written.\n");

    /* An empty tree again. */
    hr = IDCompositionVisual2_RemoveAllVisuals(root);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_Commit(device);
    ok(hr == S_OK, "Got hr %#lx.\n", hr);
    flush_events(100);
    count = (ULONG_PTR)GetPropW(hwnd, WINE_DCOMP_CHILD_COUNT_PROP);
    ok(!count, "Got %Iu leaves.\n", count);

    IDCompositionVisual2_Release(surface_visual);
    IDCompositionSurface_Release(surface);
    IDCompositionVisual3_Release(group3);
    IDCompositionVisual2_Release(group);
    IDCompositionVisual2_Release(root);
    for (i = 0; i < ARRAY_SIZE(leaf); ++i)
    {
        IDCompositionVisual2_SetContent(leaf[i], NULL);
        IDCompositionVisual2_Release(leaf[i]);
        IDXGISwapChain1_Release(swapchain[i]);
    }
    IDCompositionTarget_Release(target);
    IDCompositionDesktopDevice_Release(device);
    IDXGIDevice_Release(dxgi_device);
    ID3D11Device_Release(d3d_device);
    DestroyWindow(hwnd);
}

START_TEST(dcomp)
{
    WNDCLASSA wc = {0};

    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (const char *)IDC_ARROW);
    wc.lpszClassName = test_class;
    RegisterClassA(&wc);

    test_create_device();
    test_visual();
    test_target();
    test_surface();
    test_object_creation();
    test_leaf_serialization();

    UnregisterClassA(test_class, GetModuleHandleA(NULL));
}
