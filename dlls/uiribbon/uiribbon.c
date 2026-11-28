/*
 * Copyright 2017 Fabian Maurer
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

#include "wine/debug.h"

#define COBJMACROS

#include "uiribbon_private.h"

#include <stdio.h>

WINE_DEFAULT_DEBUG_CHANNEL(uiribbon);

static void ribbon_store_cleanup(struct ribbon_store *store);

static inline UIRibbonFrameworkImpl *impl_from_IUIFramework(IUIFramework *iface)
{
    return CONTAINING_RECORD(iface, UIRibbonFrameworkImpl, IUIFramework_iface);
}

/*** IUnknown methods ***/

static HRESULT WINAPI UIRibbonFrameworkImpl_QueryInterface(IUIFramework *iface, REFIID riid, void **ppvObject)
{
    UIRibbonFrameworkImpl *This = impl_from_IUIFramework(iface);

    TRACE("(%p/%p)->(%s,%p)\n", iface, This, debugstr_guid(riid), ppvObject);

    if (IsEqualGUID(riid, &IID_IUnknown)
        || IsEqualGUID(riid, &IID_IUIFramework))
    {
        IUnknown_AddRef(iface);
        *ppvObject = &This->IUIFramework_iface;
        return S_OK;
    }
    if (IsEqualGUID(riid, &IID_IPropertyStore))
    {
        IUnknown_AddRef(iface);
        *ppvObject = &This->framework_store.IPropertyStore_iface;
        return S_OK;
    }

    ERR("(%p)->(%s,%p),not found\n",This,debugstr_guid(riid),ppvObject);
    return E_NOINTERFACE;
}

static ULONG WINAPI UIRibbonFrameworkImpl_AddRef(IUIFramework *iface)
{
    UIRibbonFrameworkImpl *This = impl_from_IUIFramework(iface);
    ULONG ref = InterlockedIncrement(&This->ref);

    TRACE("(%p/%p)->(): new ref %ld\n", iface, This, ref);

    return ref;
}

static ULONG WINAPI UIRibbonFrameworkImpl_Release(IUIFramework *iface)
{
    UIRibbonFrameworkImpl *This = impl_from_IUIFramework(iface);
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE("(%p/%p)->(): new ref %ld\n", iface, This, ref);

    if (!ref)
    {
        if (This->application)
            IUIApplication_Release(This->application);
        ribbon_store_cleanup(&This->framework_store);
        ribbon_store_cleanup(&This->view_store);
        HeapFree(GetProcessHeap(), 0, This);
    }

    return ref;
}

/*** IPropertyStore methods ***/

/* The framework object keeps the properties of the ribbon as a whole (the UI_PKEY_Global*Color
 * values), the view those of the ribbon view (UI_PKEY_Viewable, UI_PKEY_Minimized,
 * UI_PKEY_QuickAccessToolbarDock). */

static inline struct ribbon_store *impl_from_IPropertyStore(IPropertyStore *iface)
{
    return CONTAINING_RECORD(iface, struct ribbon_store, IPropertyStore_iface);
}

static HRESULT WINAPI ribbon_store_QueryInterface(IPropertyStore *iface, REFIID riid, void **out)
{
    struct ribbon_store *store = impl_from_IPropertyStore(iface);
    return IUnknown_QueryInterface(store->outer, riid, out);
}

static ULONG WINAPI ribbon_store_AddRef(IPropertyStore *iface)
{
    struct ribbon_store *store = impl_from_IPropertyStore(iface);
    return IUnknown_AddRef(store->outer);
}

static ULONG WINAPI ribbon_store_Release(IPropertyStore *iface)
{
    struct ribbon_store *store = impl_from_IPropertyStore(iface);
    return IUnknown_Release(store->outer);
}

static struct ribbon_property *find_ribbon_property(struct ribbon_store *store, REFPROPERTYKEY key)
{
    unsigned int i;

    for (i = 0; i < store->count; ++i)
    {
        if (IsEqualGUID(&store->properties[i].key.fmtid, &key->fmtid) && store->properties[i].key.pid == key->pid)
            return &store->properties[i];
    }
    return NULL;
}

static HRESULT WINAPI ribbon_store_GetCount(IPropertyStore *iface, DWORD *count)
{
    struct ribbon_store *store = impl_from_IPropertyStore(iface);

    TRACE("(%p, %p)\n", iface, count);

    if (!count)
        return E_POINTER;

    *count = store->count;
    return S_OK;
}

static HRESULT WINAPI ribbon_store_GetAt(IPropertyStore *iface, DWORD index, PROPERTYKEY *key)
{
    struct ribbon_store *store = impl_from_IPropertyStore(iface);

    TRACE("(%p, %lu, %p)\n", iface, index, key);

    if (!key)
        return E_POINTER;
    if (index >= store->count)
        return E_INVALIDARG;

    *key = store->properties[index].key;
    return S_OK;
}

static HRESULT WINAPI ribbon_store_GetValue(IPropertyStore *iface, REFPROPERTYKEY key, PROPVARIANT *value)
{
    struct ribbon_store *store = impl_from_IPropertyStore(iface);
    struct ribbon_property *property;

    TRACE("(%p, %p, %p)\n", iface, key, value);

    if (!key || !value)
        return E_POINTER;

    PropVariantInit(value);
    if (!(property = find_ribbon_property(store, key)))
        return S_OK;

    return PropVariantCopy(value, &property->value);
}

static HRESULT WINAPI ribbon_store_SetValue(IPropertyStore *iface, REFPROPERTYKEY key, REFPROPVARIANT value)
{
    struct ribbon_store *store = impl_from_IPropertyStore(iface);
    struct ribbon_property *property;
    PROPVARIANT copy;
    HRESULT hr;

    TRACE("(%p, {%s, %lu}, %p)\n", iface, key ? debugstr_guid(&key->fmtid) : "", key ? key->pid : 0, value);

    if (!key || !value)
        return E_POINTER;

    PropVariantInit(&copy);
    if (FAILED(hr = PropVariantCopy(&copy, value)))
        return hr;

    if ((property = find_ribbon_property(store, key)))
    {
        PropVariantClear(&property->value);
        property->value = copy;
        return S_OK;
    }

    if (!(property = realloc(store->properties, (store->count + 1) * sizeof(*property))))
    {
        PropVariantClear(&copy);
        return E_OUTOFMEMORY;
    }
    store->properties = property;
    property += store->count++;
    property->key = *key;
    property->value = copy;
    return S_OK;
}

static HRESULT WINAPI ribbon_store_Commit(IPropertyStore *iface)
{
    TRACE("(%p)\n", iface);

    /* Nothing is drawn that the values would have to be applied to. */
    return S_OK;
}

static void ribbon_store_cleanup(struct ribbon_store *store)
{
    while (store->count)
        PropVariantClear(&store->properties[--store->count].value);
    free(store->properties);
    store->properties = NULL;
}

static const IPropertyStoreVtbl ribbon_store_vtbl =
{
    ribbon_store_QueryInterface,
    ribbon_store_AddRef,
    ribbon_store_Release,
    ribbon_store_GetCount,
    ribbon_store_GetAt,
    ribbon_store_GetValue,
    ribbon_store_SetValue,
    ribbon_store_Commit
};

/*** IUIRibbon methods ***/

static inline UIRibbonFrameworkImpl *impl_from_IUIRibbon(IUIRibbon *iface)
{
    return CONTAINING_RECORD(iface, UIRibbonFrameworkImpl, IUIRibbon_iface);
}

static HRESULT WINAPI UIRibbonImpl_QueryInterface(IUIRibbon *iface, REFIID riid, void **out)
{
    UIRibbonFrameworkImpl *This = impl_from_IUIRibbon(iface);

    TRACE("(%p)->(%s, %p)\n", iface, debugstr_guid(riid), out);

    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IUIRibbon))
    {
        IUIFramework_AddRef(&This->IUIFramework_iface);
        *out = iface;
        return S_OK;
    }
    if (IsEqualGUID(riid, &IID_IPropertyStore))
    {
        IUIFramework_AddRef(&This->IUIFramework_iface);
        *out = &This->view_store.IPropertyStore_iface;
        return S_OK;
    }

    WARN("(%p)->(%s, %p), not found\n", iface, debugstr_guid(riid), out);
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI UIRibbonImpl_AddRef(IUIRibbon *iface)
{
    UIRibbonFrameworkImpl *This = impl_from_IUIRibbon(iface);
    return IUIFramework_AddRef(&This->IUIFramework_iface);
}

static ULONG WINAPI UIRibbonImpl_Release(IUIRibbon *iface)
{
    UIRibbonFrameworkImpl *This = impl_from_IUIRibbon(iface);
    return IUIFramework_Release(&This->IUIFramework_iface);
}

static HRESULT WINAPI UIRibbonImpl_GetHeight(IUIRibbon *iface, UINT32 *cy)
{
    TRACE("(%p, %p)\n", iface, cy);

    if (!cy)
        return E_POINTER;

    /* No ribbon is drawn, so it takes no room in the frame window. */
    *cy = 0;
    return S_OK;
}

static HRESULT WINAPI UIRibbonImpl_LoadSettingsFromStream(IUIRibbon *iface, IStream *stream)
{
    FIXME("(%p, %p): stub!\n", iface, stream);

    return E_NOTIMPL;
}

static HRESULT WINAPI UIRibbonImpl_SaveSettingsToStream(IUIRibbon *iface, IStream *stream)
{
    FIXME("(%p, %p): stub!\n", iface, stream);

    return E_NOTIMPL;
}

static const IUIRibbonVtbl IUIRibbon_Vtbl =
{
    UIRibbonImpl_QueryInterface,
    UIRibbonImpl_AddRef,
    UIRibbonImpl_Release,
    UIRibbonImpl_GetHeight,
    UIRibbonImpl_LoadSettingsFromStream,
    UIRibbonImpl_SaveSettingsToStream
};

/*** IUIFramework methods ***/

static HRESULT WINAPI UIRibbonFrameworkImpl_Initialize(IUIFramework *iface, HWND frameWnd, IUIApplication *application)
{
    UIRibbonFrameworkImpl *This = impl_from_IUIFramework(iface);

    TRACE("(%p, %p, %p)\n", iface, frameWnd, application);

    if (!application)
        return E_POINTER;
    if (!IsWindow(frameWnd))
        return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    if (This->application)
        return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);

    IUIApplication_AddRef(application);
    This->application = application;
    This->frame = frameWnd;
    return S_OK;
}

static HRESULT WINAPI UIRibbonFrameworkImpl_Destroy(IUIFramework *iface)
{
    UIRibbonFrameworkImpl *This = impl_from_IUIFramework(iface);
    IUIApplication *application = This->application;

    TRACE("(%p)\n", iface);

    if (!application)
        return S_OK;

    This->application = NULL;
    This->frame = NULL;
    if (This->view_created)
    {
        This->view_created = FALSE;
        IUIApplication_OnViewChanged(application, 0, UI_VIEWTYPE_RIBBON,
                (IUnknown *)&This->IUIRibbon_iface, UI_VIEWVERB_DESTROY, 0);
    }
    IUIApplication_Release(application);
    return S_OK;
}

static HRESULT WINAPI UIRibbonFrameworkImpl_LoadUI(IUIFramework *iface, HINSTANCE instance, LPCWSTR resourceName)
{
    UIRibbonFrameworkImpl *This = impl_from_IUIFramework(iface);

    TRACE("(%p, %p, %s)\n", iface, instance, debugstr_w(resourceName));

    if (!This->application)
        return E_FAIL;
    if (!resourceName)
        return E_POINTER;
    if (!FindResourceW(instance, resourceName, L"UIFILE"))
    {
        WARN("No UIFILE resource %s in module %p.\n", debugstr_w(resourceName), instance);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    FIXME("The ribbon markup is not read, no commands are created and no ribbon is drawn.\n");

    if (!This->view_created)
    {
        This->view_created = TRUE;
        IUIApplication_OnViewChanged(This->application, 0, UI_VIEWTYPE_RIBBON,
                (IUnknown *)&This->IUIRibbon_iface, UI_VIEWVERB_CREATE, 0);
    }
    return S_OK;
}

static HRESULT WINAPI UIRibbonFrameworkImpl_GetView(IUIFramework *iface, UINT32 viewId, REFIID riid, void **ppv)
{
    UIRibbonFrameworkImpl *This = impl_from_IUIFramework(iface);

    TRACE("(%p, %u, %s, %p)\n", iface, viewId, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;
    *ppv = NULL;
    if (viewId || !This->view_created)
        return E_INVALIDARG;

    return IUIRibbon_QueryInterface(&This->IUIRibbon_iface, riid, ppv);
}

static HRESULT WINAPI UIRibbonFrameworkImpl_GetUICommandProperty(IUIFramework *iface, UINT32 commandId, REFPROPERTYKEY key, PROPVARIANT *value)
{
    FIXME("(%p, %u, %p, %p): stub!\n", iface, commandId, key, value);

    return E_NOTIMPL;
}

static HRESULT WINAPI UIRibbonFrameworkImpl_SetUICommandProperty(IUIFramework *iface, UINT32 commandId, REFPROPERTYKEY key, REFPROPVARIANT value)
{
    FIXME("(%p, %u, %p, %p): stub!\n", iface, commandId, key, value);

    return E_NOTIMPL;
}

static HRESULT WINAPI UIRibbonFrameworkImpl_InvalidateUICommand(IUIFramework *iface, UINT32 commandId, UI_INVALIDATIONS flags, const PROPERTYKEY *key)
{
    UIRibbonFrameworkImpl *This = impl_from_IUIFramework(iface);

    /* There is no control that would show a command's state, value or properties. */
    TRACE("(%p, %u, %#x, %p)\n", iface, commandId, flags, key);

    return This->application ? S_OK : E_FAIL;
}

static HRESULT WINAPI UIRibbonFrameworkImpl_FlushPendingInvalidations(IUIFramework *iface)
{
    UIRibbonFrameworkImpl *This = impl_from_IUIFramework(iface);

    TRACE("(%p)\n", iface);

    return This->application ? S_OK : E_FAIL;
}

static HRESULT WINAPI UIRibbonFrameworkImpl_SetModes(IUIFramework *iface, INT32 iModes)
{
    UIRibbonFrameworkImpl *This = impl_from_IUIFramework(iface);

    /* Application modes select which controls are shown. */
    TRACE("(%p, %#x)\n", iface, iModes);

    return This->application ? S_OK : E_FAIL;
}

static const IUIFrameworkVtbl IUIFramework_Vtbl =
{
    UIRibbonFrameworkImpl_QueryInterface,
    UIRibbonFrameworkImpl_AddRef,
    UIRibbonFrameworkImpl_Release,
    UIRibbonFrameworkImpl_Initialize,
    UIRibbonFrameworkImpl_Destroy,
    UIRibbonFrameworkImpl_LoadUI,
    UIRibbonFrameworkImpl_GetView,
    UIRibbonFrameworkImpl_GetUICommandProperty,
    UIRibbonFrameworkImpl_SetUICommandProperty,
    UIRibbonFrameworkImpl_InvalidateUICommand,
    UIRibbonFrameworkImpl_FlushPendingInvalidations,
    UIRibbonFrameworkImpl_SetModes
};

HRESULT UIRibbonFrameworkImpl_Create(IUnknown *pUnkOuter, void **ppObj)
{
    UIRibbonFrameworkImpl *object;

    TRACE("(%p,%p)\n", pUnkOuter, ppObj);

    object = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(UIRibbonFrameworkImpl));
    if (!object)
        return E_OUTOFMEMORY;

    object->IUIFramework_iface.lpVtbl = &IUIFramework_Vtbl;
    object->IUIRibbon_iface.lpVtbl = &IUIRibbon_Vtbl;
    object->framework_store.IPropertyStore_iface.lpVtbl = &ribbon_store_vtbl;
    object->framework_store.outer = (IUnknown *)&object->IUIFramework_iface;
    object->view_store.IPropertyStore_iface.lpVtbl = &ribbon_store_vtbl;
    object->view_store.outer = (IUnknown *)&object->IUIRibbon_iface;
    object->ref = 1;

    *ppObj = &object->IUIFramework_iface;

    return S_OK;
}

/*** IUIImage ***/

struct ui_image
{
    IUIImage IUIImage_iface;
    LONG ref;
    HBITMAP bitmap;
};

static inline struct ui_image *impl_from_IUIImage(IUIImage *iface)
{
    return CONTAINING_RECORD(iface, struct ui_image, IUIImage_iface);
}

static HRESULT WINAPI ui_image_QueryInterface(IUIImage *iface, REFIID riid, void **out)
{
    TRACE("(%p)->(%s, %p)\n", iface, debugstr_guid(riid), out);

    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IUIImage))
    {
        IUIImage_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("(%p)->(%s, %p), not found\n", iface, debugstr_guid(riid), out);
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI ui_image_AddRef(IUIImage *iface)
{
    struct ui_image *image = impl_from_IUIImage(iface);
    ULONG ref = InterlockedIncrement(&image->ref);

    TRACE("(%p): new ref %ld\n", iface, ref);

    return ref;
}

static ULONG WINAPI ui_image_Release(IUIImage *iface)
{
    struct ui_image *image = impl_from_IUIImage(iface);
    ULONG ref = InterlockedDecrement(&image->ref);

    TRACE("(%p): new ref %ld\n", iface, ref);

    if (!ref)
    {
        DeleteObject(image->bitmap);
        free(image);
    }

    return ref;
}

static HRESULT WINAPI ui_image_GetBitmap(IUIImage *iface, HBITMAP *bitmap)
{
    struct ui_image *image = impl_from_IUIImage(iface);

    TRACE("(%p, %p)\n", iface, bitmap);

    if (!bitmap)
        return E_POINTER;

    *bitmap = image->bitmap;
    return S_OK;
}

static const IUIImageVtbl ui_image_vtbl =
{
    ui_image_QueryInterface,
    ui_image_AddRef,
    ui_image_Release,
    ui_image_GetBitmap
};

/*** IUIImageFromBitmap ***/

static HRESULT WINAPI image_factory_QueryInterface(IUIImageFromBitmap *iface, REFIID riid, void **out)
{
    TRACE("(%p)->(%s, %p)\n", iface, debugstr_guid(riid), out);

    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IUIImageFromBitmap))
    {
        *out = iface;
        return S_OK;
    }

    WARN("(%p)->(%s, %p), not found\n", iface, debugstr_guid(riid), out);
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI image_factory_AddRef(IUIImageFromBitmap *iface)
{
    return 2;
}

static ULONG WINAPI image_factory_Release(IUIImageFromBitmap *iface)
{
    return 1;
}

static HRESULT WINAPI image_factory_CreateImage(IUIImageFromBitmap *iface, HBITMAP bitmap,
        UI_OWNERSHIP options, IUIImage **out)
{
    struct ui_image *image;

    TRACE("(%p, %p, %#x, %p)\n", iface, bitmap, options, out);

    if (!out)
        return E_POINTER;
    *out = NULL;
    if (GetObjectType(bitmap) != OBJ_BITMAP)
        return E_INVALIDARG;

    if (!(image = calloc(1, sizeof(*image))))
        return E_OUTOFMEMORY;

    if (options == UI_OWNERSHIP_COPY
            && !(bitmap = CopyImage(bitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION)))
    {
        free(image);
        return E_OUTOFMEMORY;
    }

    image->IUIImage_iface.lpVtbl = &ui_image_vtbl;
    image->ref = 1;
    image->bitmap = bitmap;
    *out = &image->IUIImage_iface;
    return S_OK;
}

static const IUIImageFromBitmapVtbl image_factory_vtbl =
{
    image_factory_QueryInterface,
    image_factory_AddRef,
    image_factory_Release,
    image_factory_CreateImage
};

static IUIImageFromBitmap image_factory = { &image_factory_vtbl };

HRESULT UIRibbonImageFromBitmapFactory_Create(IUnknown *pUnkOuter, void **ppObj)
{
    TRACE("(%p, %p)\n", pUnkOuter, ppObj);

    *ppObj = &image_factory;
    return S_OK;
}
