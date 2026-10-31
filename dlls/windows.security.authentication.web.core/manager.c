/* WinRT Windows.Security.Authentication.Web.Core.WebAuthenticationCoreManager Implementation
 *
 * Copyright (C) 2026 Giang Nguyen
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

#include "private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(webcore);

struct manager_statics
{
    IActivationFactory IActivationFactory_iface;
    IWebAuthenticationCoreManagerStatics IWebAuthenticationCoreManagerStatics_iface;
    IWebAuthenticationCoreManagerStatics2 IWebAuthenticationCoreManagerStatics2_iface;
    IWebAuthenticationCoreManagerStatics4 IWebAuthenticationCoreManagerStatics4_iface;
    LONG ref;
};

static inline struct manager_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct manager_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct manager_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IInspectable_AddRef( (*out = &impl->IActivationFactory_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IWebAuthenticationCoreManagerStatics ))
    {
        IInspectable_AddRef( (*out = &impl->IWebAuthenticationCoreManagerStatics_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IWebAuthenticationCoreManagerStatics2 ))
    {
        IInspectable_AddRef( (*out = &impl->IWebAuthenticationCoreManagerStatics2_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IWebAuthenticationCoreManagerStatics4 ))
    {
        IInspectable_AddRef( (*out = &impl->IWebAuthenticationCoreManagerStatics4_iface) );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct manager_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct manager_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p, ref %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    FIXME( "iface %p, instance %p stub!\n", iface, instance );
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

/* Wine does not implement any web account provider. Report "provider not found"
 * the same way Windows does for an unknown provider id, i.e. complete the
 * operation successfully with a NULL result, so that callers fall back to their
 * regular (non-WAM) sign-in path instead of failing hard. */
static HRESULT find_account_provider_callback( IUnknown *invoker, IUnknown *param, PROPVARIANT *result, BOOL called_async )
{
    result->vt = VT_UNKNOWN;
    result->punkVal = NULL;
    return S_OK;
}

static HRESULT create_null_provider_operation( IUnknown *invoker, IAsyncOperation_WebAccountProvider **operation )
{
    return async_operation_web_account_provider_create( invoker, NULL, find_account_provider_callback, operation );
}

DEFINE_IINSPECTABLE( statics, IWebAuthenticationCoreManagerStatics, struct manager_statics, IActivationFactory_iface )

static HRESULT WINAPI statics_GetTokenSilentlyAsync( IWebAuthenticationCoreManagerStatics *iface, IWebTokenRequest *request,
                                                     IAsyncOperation_WebTokenRequestResult **operation )
{
    FIXME( "iface %p, request %p, operation %p stub!\n", iface, request, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics_GetTokenSilentlyWithWebAccountAsync( IWebAuthenticationCoreManagerStatics *iface, IWebTokenRequest *request,
                                                                    IWebAccount *web_account, IAsyncOperation_WebTokenRequestResult **operation )
{
    FIXME( "iface %p, request %p, web_account %p, operation %p stub!\n", iface, request, web_account, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics_RequestTokenAsync( IWebAuthenticationCoreManagerStatics *iface, IWebTokenRequest *request,
                                                  IAsyncOperation_WebTokenRequestResult **operation )
{
    FIXME( "iface %p, request %p, operation %p stub!\n", iface, request, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics_RequestTokenWithWebAccountAsync( IWebAuthenticationCoreManagerStatics *iface, IWebTokenRequest *request,
                                                                IWebAccount *web_account, IAsyncOperation_WebTokenRequestResult **operation )
{
    FIXME( "iface %p, request %p, web_account %p, operation %p stub!\n", iface, request, web_account, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics_FindAccountAsync( IWebAuthenticationCoreManagerStatics *iface, IWebAccountProvider *provider,
                                                 HSTRING web_account_id, IAsyncOperation_WebAccount **operation )
{
    FIXME( "iface %p, provider %p, web_account_id %s, operation %p stub!\n", iface, provider,
           debugstr_hstring( web_account_id ), operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics_FindAccountProviderAsync( IWebAuthenticationCoreManagerStatics *iface, HSTRING web_account_provider_id,
                                                         IAsyncOperation_WebAccountProvider **operation )
{
    TRACE( "iface %p, web_account_provider_id %s, operation %p.\n", iface,
           debugstr_hstring( web_account_provider_id ), operation );
    return create_null_provider_operation( (IUnknown *)iface, operation );
}

static HRESULT WINAPI statics_FindAccountProviderWithAuthorityAsync( IWebAuthenticationCoreManagerStatics *iface, HSTRING web_account_provider_id,
                                                                      HSTRING authority, IAsyncOperation_WebAccountProvider **operation )
{
    TRACE( "iface %p, web_account_provider_id %s, authority %s, operation %p.\n", iface,
           debugstr_hstring( web_account_provider_id ), debugstr_hstring( authority ), operation );
    return create_null_provider_operation( (IUnknown *)iface, operation );
}

static const struct IWebAuthenticationCoreManagerStaticsVtbl statics_vtbl =
{
    statics_QueryInterface,
    statics_AddRef,
    statics_Release,
    /* IInspectable methods */
    statics_GetIids,
    statics_GetRuntimeClassName,
    statics_GetTrustLevel,
    /* IWebAuthenticationCoreManagerStatics methods */
    statics_GetTokenSilentlyAsync,
    statics_GetTokenSilentlyWithWebAccountAsync,
    statics_RequestTokenAsync,
    statics_RequestTokenWithWebAccountAsync,
    statics_FindAccountAsync,
    statics_FindAccountProviderAsync,
    statics_FindAccountProviderWithAuthorityAsync,
};

DEFINE_IINSPECTABLE( statics2, IWebAuthenticationCoreManagerStatics2, struct manager_statics, IActivationFactory_iface )

static HRESULT WINAPI statics2_FindAccountProviderWithAuthorityForUserAsync( IWebAuthenticationCoreManagerStatics2 *iface,
                                                                              HSTRING web_account_provider_id, HSTRING authority,
                                                                              IUser *user, IAsyncOperation_WebAccountProvider **operation )
{
    TRACE( "iface %p, web_account_provider_id %s, authority %s, user %p, operation %p.\n", iface,
           debugstr_hstring( web_account_provider_id ), debugstr_hstring( authority ), user, operation );
    return create_null_provider_operation( (IUnknown *)iface, operation );
}

static const struct IWebAuthenticationCoreManagerStatics2Vtbl statics2_vtbl =
{
    statics2_QueryInterface,
    statics2_AddRef,
    statics2_Release,
    /* IInspectable methods */
    statics2_GetIids,
    statics2_GetRuntimeClassName,
    statics2_GetTrustLevel,
    /* IWebAuthenticationCoreManagerStatics2 methods */
    statics2_FindAccountProviderWithAuthorityForUserAsync,
};

DEFINE_IINSPECTABLE( statics4, IWebAuthenticationCoreManagerStatics4, struct manager_statics, IActivationFactory_iface )

static HRESULT WINAPI statics4_FindAllAccountsAsync( IWebAuthenticationCoreManagerStatics4 *iface, IWebAccountProvider *provider,
                                                      IAsyncOperation_FindAllAccountsResult **operation )
{
    FIXME( "iface %p, provider %p, operation %p stub!\n", iface, provider, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics4_FindAllAccountsWithClientIdAsync( IWebAuthenticationCoreManagerStatics4 *iface, IWebAccountProvider *provider,
                                                                  HSTRING client_id, IAsyncOperation_FindAllAccountsResult **operation )
{
    FIXME( "iface %p, provider %p, client_id %s, operation %p stub!\n", iface, provider,
           debugstr_hstring( client_id ), operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI statics4_FindSystemAccountProviderAsync( IWebAuthenticationCoreManagerStatics4 *iface, HSTRING web_account_provider_id,
                                                                IAsyncOperation_WebAccountProvider **operation )
{
    TRACE( "iface %p, web_account_provider_id %s, operation %p.\n", iface,
           debugstr_hstring( web_account_provider_id ), operation );
    return create_null_provider_operation( (IUnknown *)iface, operation );
}

static HRESULT WINAPI statics4_FindSystemAccountProviderWithAuthorityAsync( IWebAuthenticationCoreManagerStatics4 *iface,
                                                                             HSTRING web_account_provider_id, HSTRING authority,
                                                                             IAsyncOperation_WebAccountProvider **operation )
{
    TRACE( "iface %p, web_account_provider_id %s, authority %s, operation %p.\n", iface,
           debugstr_hstring( web_account_provider_id ), debugstr_hstring( authority ), operation );
    return create_null_provider_operation( (IUnknown *)iface, operation );
}

static HRESULT WINAPI statics4_FindSystemAccountProviderWithAuthorityForUserAsync( IWebAuthenticationCoreManagerStatics4 *iface,
                                                                                    HSTRING web_account_provider_id, HSTRING authority,
                                                                                    IUser *user, IAsyncOperation_WebAccountProvider **operation )
{
    TRACE( "iface %p, web_account_provider_id %s, authority %s, user %p, operation %p.\n", iface,
           debugstr_hstring( web_account_provider_id ), debugstr_hstring( authority ), user, operation );
    return create_null_provider_operation( (IUnknown *)iface, operation );
}

static const struct IWebAuthenticationCoreManagerStatics4Vtbl statics4_vtbl =
{
    statics4_QueryInterface,
    statics4_AddRef,
    statics4_Release,
    /* IInspectable methods */
    statics4_GetIids,
    statics4_GetRuntimeClassName,
    statics4_GetTrustLevel,
    /* IWebAuthenticationCoreManagerStatics4 methods */
    statics4_FindAllAccountsAsync,
    statics4_FindAllAccountsWithClientIdAsync,
    statics4_FindSystemAccountProviderAsync,
    statics4_FindSystemAccountProviderWithAuthorityAsync,
    statics4_FindSystemAccountProviderWithAuthorityForUserAsync,
};

static struct manager_statics manager_statics =
{
    {&factory_vtbl},
    {&statics_vtbl},
    {&statics2_vtbl},
    {&statics4_vtbl},
    1,
};

IActivationFactory *manager_factory = &manager_statics.IActivationFactory_iface;
