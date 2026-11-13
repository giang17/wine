/*
 * Speech API (SAPI) token implementation.
 *
 * Copyright (C) 2017 Huw Davies
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
#include <stdint.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "shlobj.h"
#include "sapiddk.h"
#include "sperror.h"

#include "wine/debug.h"

#include "sapi_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(sapi);

struct data_key
{
    ISpRegDataKey ISpRegDataKey_iface;
    LONG ref;

    HKEY key;
};

static struct data_key *impl_from_ISpRegDataKey( ISpRegDataKey *iface )
{
    return CONTAINING_RECORD( iface, struct data_key, ISpRegDataKey_iface );
}

struct object_token
{
    ISpObjectTokenInit ISpObjectTokenInit_iface;
    ISpeechObjectToken ISpeechObjectToken_iface;
    LONG ref;

    ISpDataKey *data_key;
    WCHAR *token_id;
};

static struct object_token *impl_from_ISpObjectTokenInit( ISpObjectTokenInit *iface )
{
    return CONTAINING_RECORD( iface, struct object_token, ISpObjectTokenInit_iface );
}

static struct object_token *impl_from_ISpeechObjectToken( ISpeechObjectToken *iface )
{
    return CONTAINING_RECORD( iface, struct object_token, ISpeechObjectToken_iface );
}

static HRESULT WINAPI data_key_QueryInterface( ISpRegDataKey *iface, REFIID iid, void **obj )
{
    struct data_key *This = impl_from_ISpRegDataKey( iface );

    TRACE( "(%p)->(%s %p)\n", This, debugstr_guid( iid ), obj );

    if (IsEqualIID( iid, &IID_IUnknown ) ||
        IsEqualIID( iid, &IID_ISpDataKey ) ||
        IsEqualIID( iid, &IID_ISpRegDataKey ))
    {
        ISpRegDataKey_AddRef( iface );
        *obj = iface;
        return S_OK;
    }

    FIXME( "interface %s not implemented\n", debugstr_guid( iid ) );
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI data_key_AddRef( ISpRegDataKey *iface )
{
    struct data_key *This = impl_from_ISpRegDataKey( iface );
    ULONG ref = InterlockedIncrement( &This->ref );

    TRACE( "(%p) ref = %lu\n", This, ref );
    return ref;
}

static ULONG WINAPI data_key_Release( ISpRegDataKey *iface )
{
    struct data_key *This = impl_from_ISpRegDataKey( iface );
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE( "(%p) ref = %lu\n", This, ref );

    if (!ref)
    {
        if (This->key) RegCloseKey( This->key );
        free( This );
    }

    return ref;
}

static HRESULT WINAPI data_key_SetData( ISpRegDataKey *iface, LPCWSTR name,
                                        ULONG size, const BYTE *data )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_key_GetData( ISpRegDataKey *iface, LPCWSTR name,
                                        ULONG *size, BYTE *data )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_key_SetStringValue( ISpRegDataKey *iface,
                                               LPCWSTR name, LPCWSTR value )
{
    struct data_key *This = impl_from_ISpRegDataKey( iface );
    DWORD ret, size;

    TRACE( "%p, %s, %s\n", This, debugstr_w(name), debugstr_w(value) );

    if (!This->key)
        return E_HANDLE;

    size = (wcslen(value) + 1) * sizeof(WCHAR);
    ret = RegSetValueExW( This->key, name, 0, REG_SZ, (BYTE *)value, size );

    return HRESULT_FROM_WIN32(ret);
}

static HRESULT WINAPI data_key_GetStringValue( ISpRegDataKey *iface,
                                               LPCWSTR name, LPWSTR *value )
{
    struct data_key *This = impl_from_ISpRegDataKey( iface );
    DWORD ret, size;
    WCHAR *content;

    TRACE( "%p, %s, %p\n", This, debugstr_w(name), value);

    if (!This->key)
        return E_HANDLE;

    size = 0;
    ret = RegGetValueW( This->key, NULL, name, RRF_RT_REG_SZ, NULL, NULL, &size );
    if (ret != ERROR_SUCCESS)
        return SPERR_NOT_FOUND;

    content = CoTaskMemAlloc(size);
    if (!content)
        return E_OUTOFMEMORY;

    ret = RegGetValueW( This->key, NULL, name, RRF_RT_REG_SZ, NULL, content, &size );
    if (ret != ERROR_SUCCESS)
    {
        CoTaskMemFree(content);
        return HRESULT_FROM_WIN32(ret);
    }

    *value = content;
    return S_OK;
}

static HRESULT WINAPI data_key_SetDWORD( ISpRegDataKey *iface,
                                         LPCWSTR name, DWORD value )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_key_GetDWORD( ISpRegDataKey *iface,
                                         LPCWSTR name, DWORD *pdwValue )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_key_OpenKey( ISpRegDataKey *iface,
                                        LPCWSTR name, ISpDataKey **sub_key )
{
    struct data_key *This = impl_from_ISpRegDataKey( iface );
    ISpRegDataKey *spregkey;
    HRESULT hr;
    HKEY key;
    LONG ret;

    TRACE( "%p, %s, %p\n", This, debugstr_w(name), sub_key );

    ret = RegOpenKeyExW( This->key, name, 0, KEY_ALL_ACCESS, &key );
    if (ret != ERROR_SUCCESS)
        return SPERR_NOT_FOUND;

    hr = data_key_create( NULL, &IID_ISpRegDataKey, (void**)&spregkey );
    if (FAILED(hr))
    {
        RegCloseKey( key );
        return hr;
    }

    hr = ISpRegDataKey_SetKey( spregkey, key, FALSE );
    if (FAILED(hr))
    {
        RegCloseKey( key );
        ISpRegDataKey_Release( spregkey );
        return hr;
    }

    hr = ISpRegDataKey_QueryInterface( spregkey, &IID_ISpDataKey, (void**)sub_key );
    ISpRegDataKey_Release( spregkey );

    return hr;
}

static HRESULT WINAPI data_key_CreateKey( ISpRegDataKey *iface,
                                          LPCWSTR name, ISpDataKey **sub_key )
{
    struct data_key *This = impl_from_ISpRegDataKey( iface );
    ISpRegDataKey *spregkey;
    HRESULT hr;
    HKEY key;
    LONG res;

    TRACE( "%p, %s, %p\n", This, debugstr_w(name), sub_key );

    res = RegCreateKeyExW( This->key, name, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL );
    if (res != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(res);

    hr = data_key_create(NULL, &IID_ISpRegDataKey, (void**)&spregkey);
    if (SUCCEEDED(hr))
    {
        hr = ISpRegDataKey_SetKey(spregkey, key, FALSE);
        if (SUCCEEDED(hr))
            hr = ISpRegDataKey_QueryInterface(spregkey, &IID_ISpDataKey, (void**)sub_key);
        ISpRegDataKey_Release(spregkey);
    }

    return hr;
}

static HRESULT WINAPI data_key_DeleteKey( ISpRegDataKey *iface, LPCWSTR name )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_key_DeleteValue( ISpRegDataKey *iface, LPCWSTR name )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_key_EnumKeys( ISpRegDataKey *iface,
                                         ULONG index, LPWSTR *sub_key )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_key_EnumValues( ISpRegDataKey *iface,
                                           ULONG index, LPWSTR *value )
{
    struct data_key *This = impl_from_ISpRegDataKey( iface );
    DWORD ret, count, size;
    WCHAR _buffer[1];
    WCHAR *content;

    if (!This->key)
        return E_HANDLE;

    count = 0;
    size = 0;

    ret = RegEnumValueW(This->key, index, _buffer, &count, NULL, NULL, NULL, &size);
    if (ret == ERROR_NO_MORE_ITEMS)
    {
        return SPERR_NO_MORE_ITEMS;
    }
    else if (ret == ERROR_NOT_ENOUGH_MEMORY)
    {
        return E_OUTOFMEMORY;
    }
    else if (ret != ERROR_SUCCESS && ret != ERROR_MORE_DATA)
    {
        return SPERR_NOT_FOUND;
    }

    size = size + sizeof(WCHAR);
    count = size / sizeof(WCHAR);

    content = CoTaskMemAlloc(size);
    if (!content)
        return E_OUTOFMEMORY;

    ret = RegEnumValueW(This->key, index, content, &count, NULL, NULL, NULL, NULL);
    if (ret != ERROR_SUCCESS)
    {
        CoTaskMemFree(content);
        return HRESULT_FROM_WIN32(ret);
    }

    *value = content;
    return S_OK;
}

static HRESULT WINAPI data_key_SetKey( ISpRegDataKey *iface,
                                       HKEY key, BOOL read_only )
{
    struct data_key *This = impl_from_ISpRegDataKey( iface );

    TRACE( "(%p)->(%p %d)\n", This, key, read_only );

    if (This->key) return SPERR_ALREADY_INITIALIZED;

    /* read_only is ignored in Windows implementations. */
    This->key = key;
    return S_OK;
}

const struct ISpRegDataKeyVtbl data_key_vtbl =
{
    data_key_QueryInterface,
    data_key_AddRef,
    data_key_Release,
    data_key_SetData,
    data_key_GetData,
    data_key_SetStringValue,
    data_key_GetStringValue,
    data_key_SetDWORD,
    data_key_GetDWORD,
    data_key_OpenKey,
    data_key_CreateKey,
    data_key_DeleteKey,
    data_key_DeleteValue,
    data_key_EnumKeys,
    data_key_EnumValues,
    data_key_SetKey
};

HRESULT data_key_create( IUnknown *outer, REFIID iid, void **obj )
{
    struct data_key *This = malloc( sizeof(*This) );
    HRESULT hr;

    if (!This) return E_OUTOFMEMORY;
    This->ISpRegDataKey_iface.lpVtbl = &data_key_vtbl;
    This->ref = 1;
    This->key = NULL;

    hr = ISpRegDataKey_QueryInterface( &This->ISpRegDataKey_iface, iid, obj );

    ISpRegDataKey_Release( &This->ISpRegDataKey_iface );
    return hr;
}

struct token_category
{
    ISpObjectTokenCategory ISpObjectTokenCategory_iface;
    LONG ref;

    ISpRegDataKey *data_key;
    WCHAR *id;
};

static struct token_category *impl_from_ISpObjectTokenCategory( ISpObjectTokenCategory *iface )
{
    return CONTAINING_RECORD( iface, struct token_category, ISpObjectTokenCategory_iface );
}

static HRESULT WINAPI token_category_QueryInterface( ISpObjectTokenCategory *iface,
                                                     REFIID iid, void **obj )
{
    struct token_category *This = impl_from_ISpObjectTokenCategory( iface );

    TRACE( "(%p)->(%s %p)\n", This, debugstr_guid( iid ), obj );

    if (IsEqualIID( iid, &IID_IUnknown ) ||
        IsEqualIID( iid, &IID_ISpDataKey ) ||
        IsEqualIID( iid, &IID_ISpObjectTokenCategory ))
    {
        ISpObjectTokenCategory_AddRef( iface );
        *obj = iface;
        return S_OK;
    }

    FIXME( "interface %s not implemented\n", debugstr_guid( iid ) );
    *obj = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI token_category_AddRef( ISpObjectTokenCategory *iface )
{
    struct token_category *This = impl_from_ISpObjectTokenCategory( iface );
    ULONG ref = InterlockedIncrement( &This->ref );

    TRACE( "(%p) ref = %lu\n", This, ref );
    return ref;
}

static ULONG WINAPI token_category_Release( ISpObjectTokenCategory *iface )
{
    struct token_category *This = impl_from_ISpObjectTokenCategory( iface );
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE( "(%p) ref = %lu\n", This, ref );

    if (!ref)
    {
        if (This->data_key) ISpRegDataKey_Release( This->data_key );
        free( This->id );
        free( This );
    }
    return ref;
}

static HRESULT WINAPI token_category_SetData( ISpObjectTokenCategory *iface,
                                              LPCWSTR name, ULONG size,
                                              const BYTE *data )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_category_GetData( ISpObjectTokenCategory *iface,
                                              LPCWSTR name, ULONG *size,
                                              BYTE *data )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_category_SetStringValue( ISpObjectTokenCategory *iface,
                                                     LPCWSTR name, LPCWSTR value )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_category_GetStringValue( ISpObjectTokenCategory *iface,
                                                     LPCWSTR name, LPWSTR *value )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_category_SetDWORD( ISpObjectTokenCategory *iface,
                                               LPCWSTR name, DWORD value )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_category_GetDWORD( ISpObjectTokenCategory *iface,
                                               LPCWSTR name, DWORD *pdwValue )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_category_OpenKey( ISpObjectTokenCategory *iface,
                                              LPCWSTR name, ISpDataKey **sub_key )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_category_CreateKey( ISpObjectTokenCategory *iface,
                                                LPCWSTR name, ISpDataKey **sub_key )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_category_DeleteKey( ISpObjectTokenCategory *iface,
                                                LPCWSTR name )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_category_DeleteValue( ISpObjectTokenCategory *iface,
                                                  LPCWSTR name )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_category_EnumKeys( ISpObjectTokenCategory *iface,
                                               ULONG index, LPWSTR *sub_key )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_category_EnumValues( ISpObjectTokenCategory *iface,
                                                 ULONG index, LPWSTR *value )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT parse_cat_id( const WCHAR *str, HKEY *root, const WCHAR **sub_key )
{
    struct table
    {
        const WCHAR *name;
        unsigned int len;
        HKEY key;
    } table[] =
    {
#define X(s)  s, ARRAY_SIZE(s) - 1
        { X(L"HKEY_LOCAL_MACHINE\\"), HKEY_LOCAL_MACHINE },
        { X(L"HKEY_CURRENT_USER\\"), HKEY_CURRENT_USER },
        { NULL }
#undef X
    };
    struct table *ptr;
    int len = lstrlenW( str );

    for (ptr = table; ptr->name; ptr++)
    {
        if (len >= ptr->len && !wcsncmp( str, ptr->name, ptr->len ))
        {
            *root = ptr->key;
            *sub_key = str + ptr->len;
            return S_OK;
        }
    }
    return S_FALSE;
}

static HRESULT WINAPI create_data_key_with_hkey( HKEY key, ISpRegDataKey **data_key )
{
    HRESULT hr;

    if (FAILED(hr = CoCreateInstance( &CLSID_SpDataKey, NULL, CLSCTX_INPROC_SERVER,
                                      &IID_ISpRegDataKey, (void **)data_key ) ))
        return hr;

    if (FAILED(hr = ISpRegDataKey_SetKey( *data_key, key, TRUE )))
    {
        ISpRegDataKey_Release( *data_key );
        *data_key = NULL;
    }

    return hr;
}

static HRESULT WINAPI token_category_SetId( ISpObjectTokenCategory *iface,
                                            LPCWSTR id, BOOL create )
{
    struct token_category *This = impl_from_ISpObjectTokenCategory( iface );
    HKEY root, key;
    const WCHAR *subkey;
    LONG res;
    HRESULT hr;

    TRACE( "(%p)->(%s %d)\n", This, debugstr_w( id ), create );

    if (This->data_key) return SPERR_ALREADY_INITIALIZED;

    hr = parse_cat_id( id, &root, &subkey );
    if (hr != S_OK) return SPERR_INVALID_REGISTRY_KEY;

    if (create)
        res = RegCreateKeyExW( root, subkey, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL );
    else
        res = RegOpenKeyExW( root, subkey, 0, KEY_ALL_ACCESS, &key );
    if (res) return SPERR_INVALID_REGISTRY_KEY;

    if (FAILED(hr = create_data_key_with_hkey( key, &This->data_key )))
    {
        RegCloseKey( key );
        return hr;
    }

    This->id = wcsdup( id );

    return hr;
}

static HRESULT WINAPI token_category_GetId( ISpObjectTokenCategory *iface,
                                            LPWSTR *id )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_category_GetDataKey( ISpObjectTokenCategory *iface,
                                                 SPDATAKEYLOCATION location,
                                                 ISpDataKey **data_key )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

struct token_with_score
{
    ISpObjectToken *token;
    uint64_t score;
};

struct token_enum
{
    ISpObjectTokenEnumBuilder ISpObjectTokenEnumBuilder_iface;
    ISpeechObjectTokens ISpeechObjectTokens_iface;
    LONG ref;

    BOOL init;
    WCHAR *req, *opt;
    struct token_with_score *tokens;
    ULONG capacity, count;
    DWORD index;
};

static struct token_enum *impl_from_ISpObjectTokenEnumBuilder( ISpObjectTokenEnumBuilder *iface )
{
    return CONTAINING_RECORD( iface, struct token_enum, ISpObjectTokenEnumBuilder_iface );
}

static struct token_enum *impl_from_ISpeechObjectTokens( ISpeechObjectTokens *iface )
{
    return CONTAINING_RECORD( iface, struct token_enum, ISpeechObjectTokens_iface );
}

struct enum_var
{
    IEnumVARIANT IEnumVARIANT_iface;
    LONG ref;

    ISpObjectTokenEnumBuilder *token_enum;
    ULONG index;
};

static struct enum_var *impl_from_IEnumVARIANT( IEnumVARIANT *iface )
{
    return CONTAINING_RECORD( iface, struct enum_var, IEnumVARIANT_iface );
}

static HRESULT score_attributes( ISpObjectToken *token, const WCHAR *attrs,
                                 BOOL match_all, uint64_t *score );

/* The user's default token is stored as DefaultTokenId in the category's key under
 * HKEY_CURRENT_USER, for a category under HKEY_LOCAL_MACHINE as well. */
static HRESULT set_user_default_token_id( struct token_category *This, const WCHAR *id )
{
    const WCHAR *sub_key;
    HKEY root, key;
    LONG res;

    if (parse_cat_id( This->id, &root, &sub_key ) != S_OK)
        return SPERR_INVALID_REGISTRY_KEY;

    res = RegCreateKeyExW( HKEY_CURRENT_USER, sub_key, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL );
    if (!res)
    {
        res = RegSetValueExW( key, L"DefaultTokenId", 0, REG_SZ, (const BYTE *)id,
                              (wcslen( id ) + 1) * sizeof(WCHAR) );
        RegCloseKey( key );
    }
    return HRESULT_FROM_WIN32( res );
}

/* A category without a default gets the first token whose Language attribute contains the
 * user's UI language, or else its first token; a token that only shares the primary language
 * does not count. */
static HRESULT pick_default_token_id( struct token_category *This, WCHAR **id )
{
    struct data_key *this_data_key = impl_from_ISpRegDataKey( This->data_key );
    DWORD count = 0, max_subkey_size = 0, root_len, token_id_size, size, i;
    WCHAR lang[32], *token_id, *first = NULL, *match = NULL;
    ISpObjectToken *token;
    HKEY tokens_key;
    uint64_t score;
    HRESULT hr = S_OK;

    if (RegOpenKeyExW( this_data_key->key, L"Tokens", 0, KEY_READ, &tokens_key ))
        return SPERR_NOT_FOUND;

    RegQueryInfoKeyW( tokens_key, NULL, NULL, NULL, &count, &max_subkey_size, NULL,
                      NULL, NULL, NULL, NULL, NULL );
    max_subkey_size++;

    root_len = wcslen( This->id );
    token_id_size = root_len + sizeof("\\Tokens\\") + max_subkey_size;
    if (!(token_id = malloc( token_id_size * sizeof(WCHAR) )))
    {
        RegCloseKey( tokens_key );
        return E_OUTOFMEMORY;
    }
    root_len = swprintf( token_id, token_id_size, L"%ls%lsTokens\\",
                         This->id, This->id[root_len - 1] == L'\\' ? L"" : L"\\" );
    swprintf( lang, ARRAY_SIZE(lang), L"Language=%x", GetUserDefaultUILanguage() );

    for (i = 0; i < count && !match; i++)
    {
        size = max_subkey_size;
        if (RegEnumKeyExW( tokens_key, i, token_id + root_len, &size, NULL, NULL, NULL, NULL ))
            break;

        if (FAILED(hr = token_create( NULL, &IID_ISpObjectToken, (void **)&token )))
            break;
        score = 0;
        if (SUCCEEDED(ISpObjectToken_SetId( token, NULL, token_id, FALSE )))
            score_attributes( token, lang, TRUE, &score );
        ISpObjectToken_Release( token );

        if ((!first && !(first = wcsdup( token_id ))) || (score && !(match = wcsdup( token_id ))))
            hr = E_OUTOFMEMORY;
        if (FAILED(hr)) break;
    }

    if (SUCCEEDED(hr))
    {
        const WCHAR *pick = match ? match : first;

        if (!pick)
            hr = SPERR_NOT_FOUND;
        else if (!(*id = CoTaskMemAlloc( (wcslen( pick ) + 1) * sizeof(WCHAR) )))
            hr = E_OUTOFMEMORY;
        else
            wcscpy( *id, pick );
    }

    free( match );
    free( first );
    free( token_id );
    RegCloseKey( tokens_key );
    return hr;
}

static HRESULT get_default_token_id( struct token_category *This, WCHAR **id )
{
    struct data_key *this_data_key = impl_from_ISpRegDataKey( This->data_key );
    const WCHAR *sub_key;
    HKEY root;
    LONG res = ERROR_FILE_NOT_FOUND;
    WCHAR regvalue[512];
    DWORD regvalue_size = sizeof( regvalue );
    HRESULT hr;

    /* The user's choice (DefaultTokenId under HKCU) wins over the machine's
     * (DefaultTokenId under HKLM), and the DefaultDefaultTokenId is the last resort. */
    if (parse_cat_id( This->id, &root, &sub_key ) == S_OK && root == HKEY_LOCAL_MACHINE)
        res = RegGetValueW( HKEY_CURRENT_USER, sub_key, L"DefaultTokenId", RRF_RT_REG_SZ,
                            NULL, regvalue, &regvalue_size );

    if (res != ERROR_SUCCESS)
    {
        regvalue_size = sizeof( regvalue );
        res = RegGetValueW( this_data_key->key, NULL, L"DefaultTokenId", RRF_RT_REG_SZ,
                            NULL, regvalue, &regvalue_size );
    }
    if (res != ERROR_SUCCESS)
    {
        regvalue_size = sizeof( regvalue );
        res = RegGetValueW( this_data_key->key, NULL, L"DefaultDefaultTokenId", RRF_RT_REG_SZ,
                            NULL, regvalue, &regvalue_size );
    }
    if (res == ERROR_FILE_NOT_FOUND)
    {
        /* Without any of them Windows picks a token and stores it as the user's default,
         * which .NET's System.Speech then reads from the registry. */
        if ((hr = pick_default_token_id( This, id )) == S_OK)
            set_user_default_token_id( This, *id );
        return hr;
    }
    if (res != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32( res );

    if (!(*id = CoTaskMemAlloc( regvalue_size )))
        return E_OUTOFMEMORY;
    wcscpy( *id, regvalue );

    return S_OK;
}

static HRESULT WINAPI token_category_EnumTokens( ISpObjectTokenCategory *iface,
                                                 LPCWSTR req, LPCWSTR opt,
                                                 IEnumSpObjectTokens **enum_tokens )
{
    struct token_category *This = impl_from_ISpObjectTokenCategory( iface );
    ISpObjectTokenEnumBuilder *builder;
    struct data_key *this_data_key;
    HKEY tokens_key;
    DWORD count, max_subkey_size, root_len, token_id_size;
    DWORD size, i;
    WCHAR *token_id = NULL, *default_id;
    ISpObjectToken *token = NULL;
    HRESULT hr;

    TRACE( "(%p)->(%s %s %p)\n", This, debugstr_w( req ), debugstr_w( opt ), enum_tokens );

    if (!This->data_key) return SPERR_UNINITIALIZED;

    hr = CoCreateInstance( &CLSID_SpObjectTokenEnum, NULL, CLSCTX_ALL,
                           &IID_ISpObjectTokenEnumBuilder, (void **)&builder );
    if (FAILED(hr)) return hr;

    hr = ISpObjectTokenEnumBuilder_SetAttribs( builder, req, opt );
    if (FAILED(hr)) goto fail;

    this_data_key = impl_from_ISpRegDataKey( This->data_key );

    if (!RegOpenKeyExW( this_data_key->key, L"Tokens", 0, KEY_ALL_ACCESS, &tokens_key ))
    {
        RegQueryInfoKeyW( tokens_key, NULL, NULL, NULL, &count, &max_subkey_size, NULL,
                NULL, NULL, NULL, NULL, NULL );
        max_subkey_size++;

        root_len = wcslen( This->id );
        token_id_size = root_len + sizeof("\\Tokens\\") + max_subkey_size;
        token_id = malloc( token_id_size * sizeof(WCHAR) );
        if (!token_id)
        {
            hr = E_OUTOFMEMORY;
            goto fail;
        }
        root_len = swprintf( token_id, token_id_size, L"%ls%lsTokens\\",
                             This->id, This->id[root_len - 1] == L'\\' ? L"" : L"\\" );

        for ( i = 0; i < count; i++ )
        {
            size = max_subkey_size;
            hr = HRESULT_FROM_WIN32(RegEnumKeyExW( tokens_key, i, token_id + root_len, &size, NULL, NULL, NULL, NULL ));
            if (FAILED(hr)) goto fail;

            hr = token_create( NULL, &IID_ISpObjectToken, (void **)&token );
            if (FAILED(hr)) goto fail;

            hr = ISpObjectToken_SetId( token, NULL, token_id, FALSE );
            if (FAILED(hr)) goto fail;

            hr = ISpObjectTokenEnumBuilder_AddTokens( builder, 1, &token );
            if (FAILED(hr)) goto fail;
            ISpObjectToken_Release( token );
            token = NULL;
        }

        hr = ISpObjectTokenEnumBuilder_Sort( builder, NULL );
        if (FAILED(hr)) goto fail;
    }

    hr = ISpObjectTokenEnumBuilder_QueryInterface( builder, &IID_IEnumSpObjectTokens,
                                                   (void **)enum_tokens );

    /* Windows stores the default of a category without one here as well. */
    if (SUCCEEDED(hr) && SUCCEEDED(get_default_token_id( This, &default_id )))
        CoTaskMemFree( default_id );

fail:
    ISpObjectTokenEnumBuilder_Release( builder );
    if ( token ) ISpObjectToken_Release( token );
    free( token_id );
    return hr;
}

static HRESULT WINAPI token_category_SetDefaultTokenId( ISpObjectTokenCategory *iface,
                                                        LPCWSTR id )
{
    struct token_category *This = impl_from_ISpObjectTokenCategory( iface );

    TRACE( "(%p)->(%s)\n", iface, debugstr_w( id ) );

    if (!This->data_key)
        return SPERR_UNINITIALIZED;

    if (!id)
        return E_INVALIDARG;

    return set_user_default_token_id( This, id );
}

static HRESULT WINAPI token_category_GetDefaultTokenId( ISpObjectTokenCategory *iface,
                                                        LPWSTR *id )
{
    struct token_category *This = impl_from_ISpObjectTokenCategory( iface );

    TRACE( "(%p)->(%p)\n", iface, id );

    if (!This->data_key)
        return SPERR_UNINITIALIZED;

    if (!id)
        return E_POINTER;

    return get_default_token_id( This, id );
}

const struct ISpObjectTokenCategoryVtbl token_category_vtbl =
{
    token_category_QueryInterface,
    token_category_AddRef,
    token_category_Release,
    token_category_SetData,
    token_category_GetData,
    token_category_SetStringValue,
    token_category_GetStringValue,
    token_category_SetDWORD,
    token_category_GetDWORD,
    token_category_OpenKey,
    token_category_CreateKey,
    token_category_DeleteKey,
    token_category_DeleteValue,
    token_category_EnumKeys,
    token_category_EnumValues,
    token_category_SetId,
    token_category_GetId,
    token_category_GetDataKey,
    token_category_EnumTokens,
    token_category_SetDefaultTokenId,
    token_category_GetDefaultTokenId,
};

HRESULT token_category_create( IUnknown *outer, REFIID iid, void **obj )
{
    struct token_category *This = malloc( sizeof(*This) );
    HRESULT hr;

    if (!This) return E_OUTOFMEMORY;
    This->ISpObjectTokenCategory_iface.lpVtbl = &token_category_vtbl;
    This->ref = 1;
    This->data_key = NULL;
    This->id = NULL;

    hr = ISpObjectTokenCategory_QueryInterface( &This->ISpObjectTokenCategory_iface, iid, obj );

    ISpObjectTokenCategory_Release( &This->ISpObjectTokenCategory_iface );
    return hr;
}

static HRESULT WINAPI token_enum_QueryInterface( ISpObjectTokenEnumBuilder *iface,
                                                 REFIID iid, void **obj )
{
    struct token_enum *This = impl_from_ISpObjectTokenEnumBuilder( iface );

    TRACE( "(%p)->(%s %p)\n", This, debugstr_guid( iid ), obj );

    if (IsEqualIID( iid, &IID_IUnknown ) ||
        IsEqualIID( iid, &IID_IEnumSpObjectTokens ) ||
        IsEqualIID( iid, &IID_ISpObjectTokenEnumBuilder ))
        *obj = &This->ISpObjectTokenEnumBuilder_iface;
    else if (IsEqualIID( iid, &IID_IDispatch ) ||
             IsEqualIID( iid, &IID_ISpeechObjectTokens ))
        *obj = &This->ISpeechObjectTokens_iface;
    else
    {
        *obj = NULL;
        FIXME( "interface %s not implemented\n", debugstr_guid( iid ) );
        return E_NOINTERFACE;
    }

    IUnknown_AddRef( (IUnknown *)*obj );
    return S_OK;
}

static ULONG WINAPI token_enum_AddRef( ISpObjectTokenEnumBuilder *iface )
{
    struct token_enum *This = impl_from_ISpObjectTokenEnumBuilder( iface );
    ULONG ref = InterlockedIncrement( &This->ref );

    TRACE( "(%p) ref = %lu\n", This, ref );
    return ref;
}

static ULONG WINAPI token_enum_Release( ISpObjectTokenEnumBuilder *iface )
{
    struct token_enum *This = impl_from_ISpObjectTokenEnumBuilder( iface );
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE( "(%p) ref = %lu\n", This, ref );

    if (!ref)
    {
        free( This->req );
        free( This->opt );
        if (This->tokens)
        {
            ULONG i;
            for ( i = 0; i < This->count; i++ )
                if ( This->tokens[i].token )
                    ISpObjectToken_Release( This->tokens[i].token );
            free( This->tokens );
        }
        free( This );
    }

    return ref;
}

static HRESULT WINAPI token_enum_Next( ISpObjectTokenEnumBuilder *iface,
                                       ULONG num, ISpObjectToken **tokens,
                                       ULONG *fetched )
{
    struct token_enum *This = impl_from_ISpObjectTokenEnumBuilder( iface );
    ULONG i;

    TRACE( "(%p)->(%lu %p %p)\n", This, num, tokens, fetched );

    if (!This->init) return SPERR_UNINITIALIZED;
    if (!fetched && num != 1) return E_POINTER;
    if (!tokens) return E_POINTER;

    for ( i = 0; i < num && This->index < This->count; i++, This->index++ )
    {
        ISpObjectToken_AddRef( This->tokens[This->index].token );
        tokens[i] = This->tokens[This->index].token;
    }

    if (fetched) *fetched = i;

    return i == num ? S_OK : S_FALSE;
}

static HRESULT WINAPI token_enum_Skip( ISpObjectTokenEnumBuilder *iface,
                                       ULONG num )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_enum_Reset( ISpObjectTokenEnumBuilder *iface)
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_enum_Clone( ISpObjectTokenEnumBuilder *iface,
                                        IEnumSpObjectTokens **clone )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_enum_Item( ISpObjectTokenEnumBuilder *iface,
                                       ULONG index, ISpObjectToken **token )
{
    struct token_enum *This = impl_from_ISpObjectTokenEnumBuilder( iface );

    TRACE( "(%p)->(%lu %p)\n", This, index, token );

    if (!This->init) return SPERR_UNINITIALIZED;

    if (!token) return E_POINTER;
    if (index >= This->count) return SPERR_NO_MORE_ITEMS;

    ISpObjectToken_AddRef( This->tokens[index].token );
    *token = This->tokens[index].token;

    return S_OK;
}

static HRESULT WINAPI token_enum_GetCount( ISpObjectTokenEnumBuilder *iface,
                                           ULONG *count )
{
    struct token_enum *This = impl_from_ISpObjectTokenEnumBuilder( iface );

    TRACE( "(%p)->(%p)\n", This, count );

    if (!This->init) return SPERR_UNINITIALIZED;

    *count = This->count;
    return S_OK;
}

static HRESULT WINAPI token_enum_SetAttribs( ISpObjectTokenEnumBuilder *iface,
                                             LPCWSTR req, LPCWSTR opt)
{
    struct token_enum *This = impl_from_ISpObjectTokenEnumBuilder( iface );

    TRACE( "(%p)->(%s %s)\n", This, debugstr_w( req ), debugstr_w( opt ) );

    if (This->init) return SPERR_ALREADY_INITIALIZED;

    if (req)
    {
        This->req = wcsdup( req );
        if (!This->req) goto out_of_mem;
    }

    if (opt)
    {
        This->opt = wcsdup( opt );
        if (!This->opt) goto out_of_mem;
    }

    This->init = TRUE;
    return S_OK;

out_of_mem:
    free( This->req );
    return E_OUTOFMEMORY;
}

static HRESULT score_attributes( ISpObjectToken *token, const WCHAR *attrs,
                                 BOOL match_all, uint64_t *score )
{
    ISpDataKey *attrs_key;
    WCHAR *attr, *attr_ctx, *buf;
    BOOL match[64];
    unsigned int i, j;
    HRESULT hr;

    if (!attrs || !*attrs)
    {
        *score = 1;
        return S_OK;
    }
    *score = 0;

    if (FAILED(hr = ISpObjectToken_OpenKey( token, L"Attributes", &attrs_key )))
        return hr == SPERR_NOT_FOUND ? S_OK : hr;

    memset( match, 0, sizeof(match) );

    /* attrs is a semicolon-separated list of attribute clauses.
     * Each clause consists of an attribute name and an optional operator and value.
     * The meaning of a clause depends on the operator given:
     *   If no operator is given, the attribute must exist.
     *   If the operator is '=', the attribute must contain the given value.
     *   If the operator is '!=', the attribute must not exist or contain the given value.
     */
    if (!(buf = wcsdup( attrs ))) return E_OUTOFMEMORY;
    for ( attr = wcstok_s( buf, L";", &attr_ctx ), i = 0; attr && i < 64;
          attr = wcstok_s( NULL, L";", &attr_ctx ), i++ )
    {
        WCHAR *p = wcspbrk( attr, L"!=" );
        WCHAR op = p ? *p : L'\0';
        WCHAR *value = NULL, *res;
        if ( p )
        {
            if ( op == L'=' )
                value = p + 1;
            else if ( op == L'!' )
            {
                if ( *(p + 1) != L'=' )
                {
                    WARN( "invalid attr operator '!%lc'.\n", *(p + 1) );
                    hr = E_INVALIDARG;
                    goto done;
                }
                value = p + 2;
            }
            *p = L'\0';
        }

        hr = ISpDataKey_GetStringValue( attrs_key, attr, &res );
        if ( p ) *p = op;
        if (SUCCEEDED(hr))
        {
            if ( !op )
                match[i] = TRUE;
            else
            {
                WCHAR *val, *val_ctx;

                match[i] = FALSE;
                for ( val = wcstok_s( res,  L";", &val_ctx ); val && !match[i];
                      val = wcstok_s( NULL, L";", &val_ctx ) )
                    match[i] = !wcsicmp( val, value );

                if (op == L'!') match[i] = !match[i];
            }
            CoTaskMemFree( res );
        }
        else if (hr == SPERR_NOT_FOUND)
        {
            hr = S_OK;
            if (op == L'!') match[i] = TRUE;
        }
        else
            goto done;

        if ( match_all && !match[i] )
            goto done;
    }

    if ( attr )
        hr = E_INVALIDARG;
    else
    {
        /* Attributes in attrs are ordered from highest to lowest priority. */
        for ( j = 0; j < i; j++ )
            if ( match[j] )
                *score |= 1ULL << (i - 1 - j);
    }

done:
    free( buf );
    return hr;
}

static BOOL grow_tokens_array( struct token_enum *This )
{
    struct token_with_score *new_tokens;
    ULONG new_cap;

    if (This->count < This->capacity) return TRUE;

    if (This->capacity > 0)
    {
        new_cap = This->capacity * 2;
        new_tokens = realloc( This->tokens, new_cap * sizeof(*new_tokens) );
    }
    else
    {
        new_cap = 1;
        new_tokens = malloc( sizeof(*new_tokens) );
    }

    if (!new_tokens) return FALSE;

    This->tokens = new_tokens;
    This->capacity = new_cap;
    return TRUE;
}

static HRESULT WINAPI token_enum_AddTokens( ISpObjectTokenEnumBuilder *iface,
                                            ULONG num, ISpObjectToken **tokens )
{
    struct token_enum *This = impl_from_ISpObjectTokenEnumBuilder( iface );
    ULONG i;
    uint64_t score;
    HRESULT hr;

    TRACE( "(%p)->(%lu %p)\n", iface, num, tokens );

    if (!This->init) return SPERR_UNINITIALIZED;
    if (!tokens) return E_POINTER;

    for ( i = 0; i < num; i++ )
    {
        if (!tokens[i]) return E_POINTER;

        hr = score_attributes( tokens[i], This->req, TRUE, &score );
        if (FAILED(hr)) return hr;
        if (!score) continue;

        hr = score_attributes( tokens[i], This->opt, FALSE, &score );
        if (FAILED(hr)) return hr;

        if (!grow_tokens_array( This )) return E_OUTOFMEMORY;
        ISpObjectToken_AddRef( tokens[i] );
        This->tokens[This->count].token = tokens[i];
        This->tokens[This->count].score = score;
        This->count++;
    }

    return S_OK;
}

static HRESULT WINAPI token_enum_AddTokensFromDataKey( ISpObjectTokenEnumBuilder *iface,
                                                       ISpDataKey *data_key,
                                                       LPCWSTR sub_key, LPCWSTR cat_id )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_enum_AddTokensFromTokenEnum( ISpObjectTokenEnumBuilder *iface,
                                                         IEnumSpObjectTokens *token_enum )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static int __cdecl token_with_score_cmp( const void *a, const void *b )
{
    const struct token_with_score *ta = a, *tb = b;

    if (ta->score > tb->score) return -1;
    else if (ta->score < tb->score) return 1;
    else return 0;
}

static HRESULT WINAPI token_enum_Sort( ISpObjectTokenEnumBuilder *iface,
                                       LPCWSTR first )
{
    struct token_enum *This = impl_from_ISpObjectTokenEnumBuilder( iface );

    TRACE( "(%p)->(%s).\n", iface, debugstr_w(first) );

    if (!This->init) return SPERR_UNINITIALIZED;
    if (!This->tokens) return S_OK;

    if (first)
    {
        FIXME( "first != NULL is not implemented.\n" );
        return E_NOTIMPL;
    }

    if (This->opt)
        qsort( This->tokens, This->count, sizeof(*This->tokens), token_with_score_cmp );

    return S_OK;
}

const struct ISpObjectTokenEnumBuilderVtbl token_enum_vtbl =
{
    token_enum_QueryInterface,
    token_enum_AddRef,
    token_enum_Release,
    token_enum_Next,
    token_enum_Skip,
    token_enum_Reset,
    token_enum_Clone,
    token_enum_Item,
    token_enum_GetCount,
    token_enum_SetAttribs,
    token_enum_AddTokens,
    token_enum_AddTokensFromDataKey,
    token_enum_AddTokensFromTokenEnum,
    token_enum_Sort
};

static HRESULT WINAPI enum_var_QueryInterface( IEnumVARIANT *iface,
                                               REFIID iid, void **obj )
{
    struct enum_var *This = impl_from_IEnumVARIANT( iface );

    TRACE( "(%p)->(%s %p)\n", This, debugstr_guid( iid ), obj );

    if (IsEqualIID( iid, &IID_IUnknown ) ||
        IsEqualIID( iid, &IID_IEnumVARIANT ))
    {
        IEnumVARIANT_AddRef( iface );
        *obj = iface;
        return S_OK;
    }

    *obj = NULL;
    FIXME( "interface %s not implemented\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI enum_var_AddRef( IEnumVARIANT *iface )
{
    struct enum_var *This = impl_from_IEnumVARIANT( iface );
    ULONG ref = InterlockedIncrement( &This->ref );

    TRACE( "(%p) ref = %lu\n", This, ref );
    return ref;
}

static ULONG WINAPI enum_var_Release( IEnumVARIANT *iface )
{
    struct enum_var *This = impl_from_IEnumVARIANT( iface );
    ULONG ref = InterlockedDecrement( &This->ref );

    TRACE( "(%p) ref = %lu\n", This, ref );

    if (!ref)
    {
        ISpObjectTokenEnumBuilder_Release( This->token_enum );
        free( This );
    }
    return ref;
}

static HRESULT WINAPI enum_var_Next( IEnumVARIANT *iface, ULONG count,
                                     VARIANT *vars, ULONG *fetched )
{
    struct enum_var *This = impl_from_IEnumVARIANT( iface );
    ULONG i, total;
    HRESULT hr;

    TRACE( "(%p)->(%lu %p %p)\n", This, count, vars, fetched );

    if (fetched) *fetched = 0;

    if (FAILED(hr = ISpObjectTokenEnumBuilder_GetCount( This->token_enum, &total )))
        return hr;

    for ( i = 0; i < count && This->index < total; i++, This->index++ )
    {
        ISpObjectToken *token;
        IDispatch *disp;

        if (FAILED(hr = ISpObjectTokenEnumBuilder_Item( This->token_enum, This->index, &token )))
            goto fail;

        hr = ISpObjectToken_QueryInterface( token, &IID_IDispatch, (void **)&disp );
        ISpObjectToken_Release( token );
        if (FAILED(hr)) goto fail;

        VariantInit( &vars[i] );
        V_VT( &vars[i] ) = VT_DISPATCH;
        V_DISPATCH( &vars[i] ) = disp;
    }

    if (fetched) *fetched = i;
    return i == count ? S_OK : S_FALSE;

fail:
    while (i--)
        VariantClear( &vars[i] );
    return hr;
}

static HRESULT WINAPI enum_var_Skip( IEnumVARIANT *iface, ULONG count )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI enum_var_Reset( IEnumVARIANT *iface )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI enum_var_Clone( IEnumVARIANT *iface, IEnumVARIANT **new_enum )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static const IEnumVARIANTVtbl enum_var_vtbl =
{
    enum_var_QueryInterface,
    enum_var_AddRef,
    enum_var_Release,
    enum_var_Next,
    enum_var_Skip,
    enum_var_Reset,
    enum_var_Clone
};

static HRESULT WINAPI speech_tokens_QueryInterface( ISpeechObjectTokens *iface,
                                                    REFIID iid, void **obj )
{
    struct token_enum *This = impl_from_ISpeechObjectTokens( iface );

    TRACE( "(%p)->(%s %p)\n", This, debugstr_guid( iid ), obj );

    return ISpObjectTokenEnumBuilder_QueryInterface(
        &This->ISpObjectTokenEnumBuilder_iface, iid, obj );
}

static ULONG WINAPI speech_tokens_AddRef( ISpeechObjectTokens *iface )
{
    struct token_enum *This = impl_from_ISpeechObjectTokens( iface );

    TRACE( "(%p)\n", This );

    return ISpObjectTokenEnumBuilder_AddRef( &This->ISpObjectTokenEnumBuilder_iface );
}

static ULONG WINAPI speech_tokens_Release( ISpeechObjectTokens *iface )
{
    struct token_enum *This = impl_from_ISpeechObjectTokens( iface );

    TRACE( "(%p)\n", This );

    return ISpObjectTokenEnumBuilder_Release( &This->ISpObjectTokenEnumBuilder_iface );
}

static HRESULT WINAPI speech_tokens_GetTypeInfoCount( ISpeechObjectTokens *iface,
                                                      UINT *count )
{

    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI speech_tokens_GetTypeInfo( ISpeechObjectTokens *iface,
                                                 UINT index,
                                                 LCID lcid,
                                                 ITypeInfo **type_info )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI speech_tokens_GetIDsOfNames( ISpeechObjectTokens *iface,
                                                   REFIID iid,
                                                   LPOLESTR *names,
                                                   UINT count,
                                                   LCID lcid,
                                                   DISPID *dispids )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI speech_tokens_Invoke( ISpeechObjectTokens *iface,
                                            DISPID dispid,
                                            REFIID iid,
                                            LCID lcid,
                                            WORD flags,
                                            DISPPARAMS *params,
                                            VARIANT *result,
                                            EXCEPINFO *excepinfo,
                                            UINT *argerr )
{
    ITypeInfo *ti;
    HRESULT hr;

    TRACE( "(%p)->(%ld %s %#lx %#x %p %p %p %p)\n", iface, dispid,
           debugstr_guid( iid ), lcid, flags, params, result, excepinfo, argerr );

    if (FAILED(hr = get_typeinfo( ISpeechObjectTokens_tid, &ti )))
        return hr;
    hr = ITypeInfo_Invoke( ti, iface, dispid, flags, params, result, excepinfo, argerr );
    ITypeInfo_Release( ti );

    return hr;
}

static HRESULT WINAPI speech_tokens_get_Count( ISpeechObjectTokens *iface,
                                               LONG *count )
{
    struct token_enum *This = impl_from_ISpeechObjectTokens( iface );

    TRACE( "(%p)->(%p)\n", This, count );

    return ISpObjectTokenEnumBuilder_GetCount( &This->ISpObjectTokenEnumBuilder_iface, (ULONG *)count );
}

static HRESULT WINAPI speech_tokens_Item( ISpeechObjectTokens *iface,
                                          LONG index, ISpeechObjectToken **token )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI speech_tokens_get__NewEnum( ISpeechObjectTokens *iface,
                                                  IUnknown **new_enum )
{
    struct enum_var *enum_var;
    HRESULT hr;

    TRACE( "(%p)->(%p)\n", iface, new_enum );

    if (!new_enum) return E_POINTER;
    if (!(enum_var = malloc( sizeof(*enum_var) ))) return E_OUTOFMEMORY;

    enum_var->IEnumVARIANT_iface.lpVtbl = &enum_var_vtbl;
    enum_var->ref = 1;
    enum_var->index = 0;
    if (FAILED(hr = ISpeechObjectTokens_QueryInterface( iface, &IID_ISpObjectTokenEnumBuilder,
                                                       (void **)&enum_var->token_enum )))
    {
        free( enum_var );
        return hr;
    }

    *new_enum = (IUnknown *)&enum_var->IEnumVARIANT_iface;

    return S_OK;
}

static const ISpeechObjectTokensVtbl speech_tokens_vtbl =
{
    speech_tokens_QueryInterface,
    speech_tokens_AddRef,
    speech_tokens_Release,
    speech_tokens_GetTypeInfoCount,
    speech_tokens_GetTypeInfo,
    speech_tokens_GetIDsOfNames,
    speech_tokens_Invoke,
    speech_tokens_get_Count,
    speech_tokens_Item,
    speech_tokens_get__NewEnum
};

HRESULT token_enum_create( IUnknown *outer, REFIID iid, void **obj )
{
    struct token_enum *This = malloc( sizeof(*This) );
    HRESULT hr;

    if (!This) return E_OUTOFMEMORY;
    This->ISpObjectTokenEnumBuilder_iface.lpVtbl = &token_enum_vtbl;
    This->ISpeechObjectTokens_iface.lpVtbl = &speech_tokens_vtbl;
    This->ref = 1;
    This->req = NULL;
    This->opt = NULL;
    This->init = FALSE;
    This->tokens = NULL;
    This->capacity = This->count = 0;
    This->index = 0;

    hr = ISpObjectTokenEnumBuilder_QueryInterface( &This->ISpObjectTokenEnumBuilder_iface, iid, obj );

    ISpObjectTokenEnumBuilder_Release( &This->ISpObjectTokenEnumBuilder_iface );
    return hr;
}

static HRESULT WINAPI token_QueryInterface( ISpObjectTokenInit *iface,
                                            REFIID iid, void **obj )
{
    struct object_token *This = impl_from_ISpObjectTokenInit( iface );

    TRACE( "(%p)->(%s %p)\n", This, debugstr_guid( iid ), obj );

    if (IsEqualIID( iid, &IID_IUnknown ) ||
        IsEqualIID( iid, &IID_ISpDataKey ) ||
        IsEqualIID( iid, &IID_ISpObjectToken ) ||
        IsEqualIID( iid, &IID_ISpObjectTokenInit ))
        *obj = &This->ISpObjectTokenInit_iface;
    else if (IsEqualIID( iid, &IID_IDispatch ) ||
             IsEqualIID( iid, &IID_ISpeechObjectToken ))
        *obj = &This->ISpeechObjectToken_iface;
    else
    {
        *obj = NULL;
        FIXME( "interface %s not implemented\n", debugstr_guid( iid ) );
        return E_NOINTERFACE;
    }

    IUnknown_AddRef( (IUnknown *)*obj );
    return S_OK;
}

static ULONG WINAPI token_AddRef( ISpObjectTokenInit *iface )
{
    struct object_token *This = impl_from_ISpObjectTokenInit( iface );
    ULONG ref = InterlockedIncrement( &This->ref );

    TRACE( "(%p) ref = %lu\n", This, ref );
    return ref;
}

static ULONG WINAPI token_Release( ISpObjectTokenInit *iface )
{
    struct object_token *This = impl_from_ISpObjectTokenInit( iface );
    ULONG ref = InterlockedDecrement(&This->ref);

    TRACE( "(%p) ref = %lu\n", This, ref );

    if (!ref)
    {
        if (This->data_key) ISpDataKey_Release( This->data_key );
        free( This->token_id );
        free( This );
    }

    return ref;
}

static HRESULT WINAPI token_SetData( ISpObjectTokenInit *iface,
                                     LPCWSTR name, ULONG size,
                                     const BYTE *data )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_GetData( ISpObjectTokenInit *iface,
                                     LPCWSTR name, ULONG *size,
                                     BYTE *data )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_SetStringValue( ISpObjectTokenInit *iface,
                                            LPCWSTR name, LPCWSTR value )
{
    struct object_token *This = impl_from_ISpObjectTokenInit( iface );

    TRACE( "%p, %s, %s\n", This, debugstr_w(name), debugstr_w(value) );

    return ISpDataKey_SetStringValue( This->data_key, name, value );
}

static HRESULT WINAPI token_GetStringValue( ISpObjectTokenInit *iface,
                                            LPCWSTR name, LPWSTR *value )
{
    struct object_token *This = impl_from_ISpObjectTokenInit( iface );

    TRACE( "%p, %s, %p\n", This, debugstr_w(name), value );

    return ISpDataKey_GetStringValue( This->data_key, name, value );
}

static HRESULT WINAPI token_SetDWORD( ISpObjectTokenInit *iface,
                                      LPCWSTR name, DWORD value )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_GetDWORD( ISpObjectTokenInit *iface,
                                      LPCWSTR name, DWORD *pdwValue )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_OpenKey( ISpObjectTokenInit *iface,
                                     LPCWSTR name, ISpDataKey **sub_key )
{
    struct object_token *This = impl_from_ISpObjectTokenInit( iface );

    TRACE( "%p, %s, %p\n", This, debugstr_w(name), sub_key );

    return ISpDataKey_OpenKey( This->data_key, name, sub_key );
}

static HRESULT WINAPI token_CreateKey( ISpObjectTokenInit *iface,
                                       LPCWSTR name, ISpDataKey **sub_key )
{
    struct object_token *This = impl_from_ISpObjectTokenInit( iface );

    TRACE( "%p, %s, %p\n", iface, debugstr_w(name), sub_key );

    return ISpDataKey_CreateKey( This->data_key, name, sub_key );
}

static HRESULT WINAPI token_DeleteKey( ISpObjectTokenInit *iface,
                                       LPCWSTR name )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_DeleteValue( ISpObjectTokenInit *iface,
                                         LPCWSTR name )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_EnumKeys( ISpObjectTokenInit *iface,
                                      ULONG index, LPWSTR *sub_key )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_EnumValues( ISpObjectTokenInit *iface,
                                        ULONG index, LPWSTR *value )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_SetId( ISpObjectTokenInit *iface,
                                   LPCWSTR category_id, LPCWSTR token_id,
                                   BOOL create )
{
    struct object_token *This = impl_from_ISpObjectTokenInit( iface );
    ISpRegDataKey *data_key;
    BOOL res;
    HRESULT hr;
    HKEY root, key;
    const WCHAR *subkey;

    TRACE( "(%p)->(%s %s %d)\n", This, debugstr_w( category_id ),
           debugstr_w(token_id), create );

    if (This->data_key) return SPERR_ALREADY_INITIALIZED;

    if (!token_id) return E_POINTER;

    hr = parse_cat_id( token_id, &root, &subkey );
    if (hr != S_OK) return SPERR_NOT_FOUND;

    if (create)
        res = RegCreateKeyExW( root, subkey, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL);
    else
        res = RegOpenKeyExW( root, subkey, 0, KEY_ALL_ACCESS, &key );
    if (res) return SPERR_NOT_FOUND;

    hr = create_data_key_with_hkey( key, &data_key );
    if (FAILED(hr))
    {
        RegCloseKey( key );
        return hr;
    }
    This->data_key = (ISpDataKey *)data_key;

    This->token_id = wcsdup(token_id);

    return S_OK;
}

static HRESULT WINAPI token_GetId( ISpObjectTokenInit *iface,
                                   LPWSTR *token_id )
{
    struct object_token *This = impl_from_ISpObjectTokenInit( iface );

    TRACE( "%p, %p\n", This, token_id);

    if (!This->data_key)
        return SPERR_UNINITIALIZED;

    if (!token_id)
        return E_POINTER;

    if (!This->token_id)
    {
        FIXME("Loading default category not supported.\n");
        return E_POINTER;
    }

    *token_id = CoTaskMemAlloc( (wcslen(This->token_id) + 1) * sizeof(WCHAR));
    if (!*token_id)
        return E_OUTOFMEMORY;

    wcscpy(*token_id, This->token_id);
    return S_OK;
}

static HRESULT WINAPI token_GetCategory( ISpObjectTokenInit *iface,
                                         ISpObjectTokenCategory **category )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_CreateInstance( ISpObjectTokenInit *iface,
                                            IUnknown *outer,
                                            DWORD class_context,
                                            REFIID riid,
                                            void **object )
{
    WCHAR *clsid_str;
    CLSID clsid;
    IUnknown *unk;
    ISpObjectWithToken *obj_token_iface;
    HRESULT hr;

    TRACE( "%p, %p, %#lx, %s, %p\n", iface, outer, class_context, debugstr_guid( riid ), object );

    if (FAILED(hr = ISpObjectTokenInit_GetStringValue( iface, L"CLSID", &clsid_str )))
        return hr;

    hr = CLSIDFromString( clsid_str, &clsid );
    CoTaskMemFree( clsid_str );
    if (FAILED(hr))
        return hr;

    if (FAILED(hr = CoCreateInstance( &clsid, outer, class_context, &IID_IUnknown, (void **)&unk )))
        return hr;

    /* Call ISpObjectWithToken::SetObjectToken if the interface is available. */
    if (SUCCEEDED(IUnknown_QueryInterface( unk, &IID_ISpObjectWithToken, (void **)&obj_token_iface )))
    {
        hr = ISpObjectWithToken_SetObjectToken( obj_token_iface, (ISpObjectToken *)iface );
        ISpObjectWithToken_Release( obj_token_iface );
        if (FAILED(hr))
            goto done;
    }

    hr = IUnknown_QueryInterface( unk, riid, object );

done:
    IUnknown_Release( unk );
    return hr;
}

/* A stored file path either is absolute or starts with "%<CSIDL in hex>%",
 * which stands for that shell folder. */
static HRESULT expand_storage_path( const WCHAR *stored, WCHAR **filepath )
{
    WCHAR folder[MAX_PATH] = L"";
    const WCHAR *rest = stored, *end;
    HRESULT hr;
    size_t len;

    if (stored[0] == '%' && (end = wcschr( stored + 1, '%' )))
    {
        ULONG csidl = wcstoul( stored + 1, NULL, 16 );

        if (FAILED(hr = SHGetFolderPathW( NULL, csidl | CSIDL_FLAG_CREATE, NULL, SHGFP_TYPE_CURRENT, folder )))
            return hr;
        rest = end + 1;
    }

    len = wcslen( folder ) + wcslen( rest ) + 1;
    if (!(*filepath = CoTaskMemAlloc( len * sizeof(WCHAR) )))
        return E_OUTOFMEMORY;
    wcscpy( *filepath, folder );
    wcscat( *filepath, rest );
    return S_OK;
}

static HRESULT create_storage_file( const WCHAR *path )
{
    WCHAR *dir, *p;
    HANDLE file;

    if (!(dir = wcsdup( path ))) return E_OUTOFMEMORY;
    for (p = wcschr( dir, '\\' ); p; p = wcschr( p + 1, '\\' ))
    {
        if (p == dir || p[-1] == ':') continue;
        *p = 0;
        CreateDirectoryW( dir, NULL );
        *p = '\\';
    }
    free( dir );

    /* An existing file is kept as it is. */
    file = CreateFileW( path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle( file );
    else if (GetLastError() != ERROR_FILE_EXISTS)
        return HRESULT_FROM_WIN32( GetLastError() );
    return S_OK;
}

static HRESULT WINAPI token_GetStorageFileName( ISpObjectTokenInit *iface,
                                                REFCLSID caller,
                                                LPCWSTR key_name,
                                                LPCWSTR filename,
                                                ULONG folder,
                                                LPWSTR *filepath )
{
    struct object_token *This = impl_from_ISpObjectTokenInit( iface );
    ISpDataKey *caller_key, *files_key;
    WCHAR clsid[39], name[40], *stored;
    size_t len;
    HRESULT hr;
    GUID guid;

    TRACE( "%p, %s, %s, %s, %#lx, %p\n", iface, debugstr_guid(caller), debugstr_w(key_name),
           debugstr_w(filename), folder, filepath );

    if (!This->data_key) return SPERR_UNINITIALIZED;
    if (!filepath) return E_POINTER;
    if (!key_name) return E_INVALIDARG;

    /* The path is kept in <token>\<caller CLSID>\Files, under key_name. */
    StringFromGUID2( caller, clsid, ARRAY_SIZE(clsid) );
    if (FAILED(hr = ISpDataKey_CreateKey( This->data_key, clsid, &caller_key )))
        return hr;
    hr = ISpDataKey_CreateKey( caller_key, L"Files", &files_key );
    ISpDataKey_Release( caller_key );
    if (FAILED(hr)) return hr;

    if (SUCCEEDED(hr = ISpDataKey_GetStringValue( files_key, key_name, &stored )))
    {
        hr = expand_storage_path( stored, filepath );
        CoTaskMemFree( stored );
        ISpDataKey_Release( files_key );
        return hr;
    }
    if (hr != SPERR_NOT_FOUND || !folder)
    {
        ISpDataKey_Release( files_key );
        return hr;
    }

    /* Not stored yet: the new file goes to "<folder>\Microsoft\Speech\Files\<filename>",
     * with a generated name unless filename names a file. */
    name[0] = 0;
    if (!filename || !*filename || filename[wcslen( filename ) - 1] == '\\')
    {
        CoCreateGuid( &guid );
        swprintf( name, ARRAY_SIZE(name), L"SP_%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X.dat",
                  guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2],
                  guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7] );
    }

    len = 64 + (filename ? wcslen( filename ) : 0) + wcslen( name );
    if (!(stored = malloc( len * sizeof(WCHAR) )))
    {
        ISpDataKey_Release( files_key );
        return E_OUTOFMEMORY;
    }
    swprintf( stored, len, L"%%%lx%%\\Microsoft\\Speech\\Files\\%s%s",
              folder & ~CSIDL_FLAG_MASK, filename ? filename : L"", name );

    if (SUCCEEDED(hr = expand_storage_path( stored, filepath )))
    {
        if (SUCCEEDED(hr = create_storage_file( *filepath )) &&
            SUCCEEDED(hr = ISpDataKey_SetStringValue( files_key, key_name, stored )))
            hr = S_FALSE; /* tells the caller that the file is new and empty */
        if (FAILED(hr))
        {
            CoTaskMemFree( *filepath );
            *filepath = NULL;
        }
    }

    free( stored );
    ISpDataKey_Release( files_key );
    return hr;
}

static HRESULT WINAPI token_RemoveStorageFileName( ISpObjectTokenInit *iface,
                                                   REFCLSID caller,
                                                   LPCWSTR key_name,
                                                   BOOL delete_file )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_Remove( ISpObjectTokenInit *iface,
                                    REFCLSID caller )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_IsUISupported( ISpObjectTokenInit *iface,
                                           LPCWSTR ui_type,
                                           void *extra_data,
                                           ULONG extra_data_size,
                                           IUnknown *object,
                                           BOOL *supported )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_DisplayUI( ISpObjectTokenInit *iface,
                                       HWND parent,
                                       LPCWSTR title,
                                       LPCWSTR ui_type,
                                       void *extra_data,
                                       ULONG extra_data_size,
                                       IUnknown *object )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI token_MatchesAttributes( ISpObjectTokenInit *iface,
                                               LPCWSTR attributes,
                                               BOOL *matches )
{
    struct object_token *This = impl_from_ISpObjectTokenInit( iface );
    uint64_t score;
    HRESULT hr;

    TRACE( "%p, %s, %p\n", iface, debugstr_w(attributes), matches );

    if (!This->data_key) return SPERR_UNINITIALIZED;
    if (!matches) return E_INVALIDARG;

    if (FAILED(hr = score_attributes( (ISpObjectToken *)iface, attributes, TRUE, &score )))
        return hr;

    *matches = !!score;
    return S_OK;
}

static HRESULT WINAPI token_InitFromDataKey( ISpObjectTokenInit *iface,
                                             LPCWSTR category_id,
                                             LPCWSTR token_id,
                                             ISpDataKey *data_key )
{
    struct object_token *This = impl_from_ISpObjectTokenInit( iface );

    TRACE( "%p, %s, %s, %p\n", iface, debugstr_w(category_id), debugstr_w(token_id), data_key );

    if (This->data_key) return SPERR_ALREADY_INITIALIZED;
    if (!token_id || !data_key) return E_POINTER;

    if (!(This->token_id = wcsdup( token_id ))) return E_OUTOFMEMORY;
    ISpDataKey_AddRef( data_key );
    This->data_key = data_key;
    return S_OK;
}

const struct ISpObjectTokenInitVtbl token_vtbl =
{
    token_QueryInterface,
    token_AddRef,
    token_Release,
    token_SetData,
    token_GetData,
    token_SetStringValue,
    token_GetStringValue,
    token_SetDWORD,
    token_GetDWORD,
    token_OpenKey,
    token_CreateKey,
    token_DeleteKey,
    token_DeleteValue,
    token_EnumKeys,
    token_EnumValues,
    token_SetId,
    token_GetId,
    token_GetCategory,
    token_CreateInstance,
    token_GetStorageFileName,
    token_RemoveStorageFileName,
    token_Remove,
    token_IsUISupported,
    token_DisplayUI,
    token_MatchesAttributes,
    token_InitFromDataKey
};

static HRESULT WINAPI speech_token_QueryInterface( ISpeechObjectToken *iface,
                                                   REFIID iid, void **obj )
{
    struct object_token *This = impl_from_ISpeechObjectToken( iface );

    TRACE( "(%p)->(%s %p)\n", This, debugstr_guid( iid ), obj );

    return ISpObjectTokenInit_QueryInterface( &This->ISpObjectTokenInit_iface, iid, obj );
}

static ULONG WINAPI speech_token_AddRef( ISpeechObjectToken *iface )
{
    struct object_token *This = impl_from_ISpeechObjectToken( iface );

    TRACE( "(%p)\n", This );

    return ISpObjectTokenInit_AddRef( &This->ISpObjectTokenInit_iface );
}

static ULONG WINAPI speech_token_Release( ISpeechObjectToken *iface )
{
    struct object_token *This = impl_from_ISpeechObjectToken( iface );

    TRACE( "(%p)\n", This );

    return ISpObjectTokenInit_Release( &This->ISpObjectTokenInit_iface );
}

static HRESULT WINAPI speech_token_GetTypeInfoCount( ISpeechObjectToken *iface,
                                                     UINT *count )
{

    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI speech_token_GetTypeInfo( ISpeechObjectToken *iface,
                                                UINT index,
                                                LCID lcid,
                                                ITypeInfo **type_info )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI speech_token_GetIDsOfNames( ISpeechObjectToken *iface,
                                                  REFIID iid,
                                                  LPOLESTR *names,
                                                  UINT count,
                                                  LCID lcid,
                                                  DISPID *dispids )
{
    ITypeInfo *ti;
    HRESULT hr;

    TRACE( "(%p)->(%s %p %u %#lx %p)\n",
           iface, debugstr_guid( iid ), names, count, lcid, dispids );

    if (FAILED(hr = get_typeinfo( ISpeechObjectToken_tid, &ti )))
        return hr;
    hr = ITypeInfo_GetIDsOfNames( ti, names, count, dispids );
    ITypeInfo_Release( ti );

    return hr;
}

static HRESULT WINAPI speech_token_Invoke( ISpeechObjectToken *iface,
                                           DISPID dispid,
                                           REFIID iid,
                                           LCID lcid,
                                           WORD flags,
                                           DISPPARAMS *params,
                                           VARIANT *result,
                                           EXCEPINFO *excepinfo,
                                           UINT *argerr )
{
    ITypeInfo *ti;
    HRESULT hr;

    TRACE( "(%p)->(%ld %s %#lx %#x %p %p %p %p)\n", iface, dispid,
           debugstr_guid( iid ), lcid, flags, params, result, excepinfo, argerr );

    if (FAILED(hr = get_typeinfo( ISpeechObjectToken_tid, &ti )))
        return hr;
    hr = ITypeInfo_Invoke( ti, iface, dispid, flags, params, result, excepinfo, argerr );
    ITypeInfo_Release( ti );

    return hr;
}

static HRESULT WINAPI speech_token_get_Id( ISpeechObjectToken *iface,
                                           BSTR *id )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI speech_token_get_DataKey( ISpeechObjectToken *iface,
                                                ISpeechDataKey **key )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI speech_token_get_Category( ISpeechObjectToken *iface,
                                                ISpeechObjectTokenCategory **cat )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI speech_token_GetDescription( ISpeechObjectToken *iface,
                                                   LONG locale, BSTR *desc )
{
    struct object_token *This = impl_from_ISpeechObjectToken( iface );
    WCHAR langid[5];
    WCHAR *desc_wstr = NULL;
    HRESULT hr;

    TRACE( "(%p)->(%#lx %p)\n", This, locale, desc );

    if (!desc) return E_POINTER;

    swprintf( langid, ARRAY_SIZE( langid ), L"%X", LANGIDFROMLCID( locale ) );

    hr = ISpObjectTokenInit_GetStringValue( &This->ISpObjectTokenInit_iface, langid, &desc_wstr );
    if (hr == SPERR_NOT_FOUND)
        hr = ISpObjectTokenInit_GetStringValue( &This->ISpObjectTokenInit_iface, NULL, &desc_wstr );
    if (FAILED(hr))
        return hr;

    *desc = SysAllocString( desc_wstr );

    CoTaskMemFree( desc_wstr );
    return *desc ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI speech_token_SetId( ISpeechObjectToken *iface,
                                          BSTR id, BSTR category_id,
                                          VARIANT_BOOL create )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI speech_token_GetAttribute( ISpeechObjectToken *iface,
                                                 BSTR name, BSTR *value )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI speech_token_CreateInstance( ISpeechObjectToken *iface,
                                                   IUnknown *outer,
                                                   SpeechTokenContext clsctx,
                                                   IUnknown **object )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI speech_token_Remove( ISpeechObjectToken *iface,
                                           BSTR clsid )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI speech_token_GetStorageFileName( ISpeechObjectToken *iface,
                                                       BSTR clsid,
                                                       BSTR key,
                                                       BSTR name,
                                                       SpeechTokenShellFolder folder,
                                                       BSTR *path )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI speech_token_RemoveStorageFileName( ISpeechObjectToken *iface,
                                                          BSTR clsid,
                                                          BSTR key,
                                                          VARIANT_BOOL remove )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI speech_token_IsUISupported( ISpeechObjectToken *iface,
                                                  const BSTR type,
                                                  const VARIANT *data,
                                                  IUnknown *object,
                                                  VARIANT_BOOL *supported )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI speech_token_DisplayUI( ISpeechObjectToken *iface,
                                              LONG hwnd,
                                              BSTR title,
                                              const BSTR type,
                                              const VARIANT *data,
                                              IUnknown *object )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

static HRESULT WINAPI speech_token_MatchesAttributes( ISpeechObjectToken *iface,
                                                      const BSTR attributes,
                                                      VARIANT_BOOL *matches )
{
    FIXME( "stub\n" );
    return E_NOTIMPL;
}

const struct ISpeechObjectTokenVtbl speech_token_vtbl =
{
    speech_token_QueryInterface,
    speech_token_AddRef,
    speech_token_Release,
    speech_token_GetTypeInfoCount,
    speech_token_GetTypeInfo,
    speech_token_GetIDsOfNames,
    speech_token_Invoke,
    speech_token_get_Id,
    speech_token_get_DataKey,
    speech_token_get_Category,
    speech_token_GetDescription,
    speech_token_SetId,
    speech_token_GetAttribute,
    speech_token_CreateInstance,
    speech_token_Remove,
    speech_token_GetStorageFileName,
    speech_token_RemoveStorageFileName,
    speech_token_IsUISupported,
    speech_token_DisplayUI,
    speech_token_MatchesAttributes
};

HRESULT token_create( IUnknown *outer, REFIID iid, void **obj )
{
    struct object_token *This = malloc( sizeof(*This) );
    HRESULT hr;

    if (!This) return E_OUTOFMEMORY;
    This->ISpObjectTokenInit_iface.lpVtbl = &token_vtbl;
    This->ISpeechObjectToken_iface.lpVtbl = &speech_token_vtbl;
    This->ref = 1;

    This->data_key = NULL;
    This->token_id = NULL;

    hr = ISpObjectTokenInit_QueryInterface( &This->ISpObjectTokenInit_iface, iid, obj );

    ISpObjectTokenInit_Release( &This->ISpObjectTokenInit_iface );
    return hr;
}
