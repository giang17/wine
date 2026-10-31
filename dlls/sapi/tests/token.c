/*
 * Speech API (SAPI) token tests.
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

#define COBJMACROS

#include "initguid.h"
#include "sapiddk.h"
#include "shlobj.h"
#include "sperror.h"

#include "wine/test.h"

DEFINE_GUID(GUID_NULL,0,0,0,0,0,0,0,0,0,0,0);

static void test_data_key(void)
{
    ISpRegDataKey *data_key;
    ISpDataKey *sub;
    HRESULT hr;
    HKEY key;
    LONG res;
    WCHAR *value = NULL;

    hr = CoCreateInstance( &CLSID_SpDataKey, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpRegDataKey, (void **)&data_key );
    ok( hr == S_OK, "got %08lx\n", hr );

    res = RegCreateKeyExA( HKEY_CURRENT_USER, "Software\\Winetest\\sapi", 0, NULL, 0, KEY_ALL_ACCESS,
                           NULL, &key, NULL );
    ok( res == ERROR_SUCCESS, "got %ld\n", res );

    hr = ISpRegDataKey_CreateKey( data_key, L"Testing", &sub );
    ok( hr == E_HANDLE, "got %08lx\n", hr );

    hr = ISpRegDataKey_GetStringValue( data_key, L"Voice", &value );
    ok( hr == E_HANDLE, "got %08lx\n", hr );

    hr = ISpRegDataKey_SetStringValue( data_key, L"Voice", L"Test" );
    ok( hr == E_HANDLE, "got %08lx\n", hr );

    hr = ISpRegDataKey_EnumValues(data_key, 0, &value);
    ok( hr == E_HANDLE, "got %08lx\n", hr );

    hr = ISpRegDataKey_SetKey( data_key, key, FALSE );
    ok( hr == S_OK, "got %08lx\n", hr );
    hr = ISpRegDataKey_SetKey( data_key, key, FALSE );
    ok( hr == SPERR_ALREADY_INITIALIZED, "got %08lx\n", hr );

    hr = ISpRegDataKey_EnumValues(data_key, 0, &value);
    ok( hr == SPERR_NO_MORE_ITEMS, "got %08lx\n", hr );

    hr = ISpRegDataKey_GetStringValue( data_key, L"Voice", &value );
    ok( hr == SPERR_NOT_FOUND, "got %08lx\n", hr );

    hr = ISpRegDataKey_GetStringValue( data_key, L"", &value );
    ok( hr == SPERR_NOT_FOUND, "got %08lx\n", hr );

    hr = ISpRegDataKey_SetStringValue( data_key, L"Voice", L"Test" );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpRegDataKey_GetStringValue( data_key, L"Voice", &value );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( !wcscmp( value, L"Test" ), "got %s\n", wine_dbgstr_w(value) );
    CoTaskMemFree( value );

    hr = ISpRegDataKey_EnumValues(data_key, 0, &value);
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( !wcscmp( value, L"Voice" ), "got %s\n", wine_dbgstr_w(value) );
    CoTaskMemFree( value );

    hr = ISpRegDataKey_EnumValues(data_key, 1, &value);
    ok( hr == SPERR_NO_MORE_ITEMS, "got %08lx\n", hr );

    hr = ISpRegDataKey_OpenKey( data_key, L"Testing", &sub );
    ok( hr == SPERR_NOT_FOUND, "got %08lx\n", hr );

    hr = ISpRegDataKey_CreateKey( data_key, L"Testing", &sub );
    ok( hr == S_OK, "got %08lx\n", hr );
    ISpDataKey_Release(sub);

    hr = ISpRegDataKey_OpenKey( data_key, L"Testing", &sub );
    ok( hr == S_OK, "got %08lx\n", hr );
    ISpDataKey_Release(sub);

    ISpRegDataKey_Release( data_key );

    hr = CoCreateInstance( &CLSID_SpDataKey, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpRegDataKey, (void **)&data_key );
    ok( hr == S_OK, "got %08lx\n", hr );

    res = RegOpenKeyExA( HKEY_CURRENT_USER, "Software\\Winetest\\sapi", 0, KEY_ALL_ACCESS, &key );
    ok( res == ERROR_SUCCESS, "got %ld\n", res );

    hr = ISpRegDataKey_SetKey( data_key, key, TRUE );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpRegDataKey_SetStringValue( data_key, L"Voice2", L"Test2" );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpRegDataKey_GetStringValue( data_key, L"Voice2", &value );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( !wcscmp( value, L"Test2" ), "got %s\n", wine_dbgstr_w(value) );
    CoTaskMemFree( value );

    hr = ISpRegDataKey_CreateKey( data_key, L"Testing2", &sub );
    ok( hr == S_OK, "got %08lx\n", hr );
    ISpDataKey_Release(sub);

    ISpRegDataKey_Release( data_key );
}

static void setup_test_voice_tokens(void)
{
    HKEY key;
    ISpRegDataKey *data_key;
    ISpDataKey *attrs_key;
    LSTATUS ret;
    HRESULT hr;

    ret = RegCreateKeyExA( HKEY_CURRENT_USER, "Software\\Winetest\\sapi\\TestVoices\\Tokens", 0, NULL, 0,
                           KEY_ALL_ACCESS, NULL, &key, NULL );
    ok( ret == ERROR_SUCCESS, "got %ld\n", ret );

    hr = CoCreateInstance( &CLSID_SpDataKey, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpRegDataKey, (void **)&data_key );
    ok( hr == S_OK, "got %08lx\n", hr );
    hr = ISpRegDataKey_SetKey( data_key, key, FALSE );
    ok( hr == S_OK, "got %08lx\n", hr );

    ISpRegDataKey_CreateKey( data_key, L"Voice1\\Attributes", &attrs_key );
    ISpDataKey_SetStringValue( attrs_key, L"Language", L"409" );
    ISpDataKey_SetStringValue( attrs_key, L"Gender", L"Female" );
    ISpDataKey_SetStringValue( attrs_key, L"Age", L"Child" );
    ISpDataKey_SetStringValue( attrs_key, L"Vendor", L"Vendor2" );
    ISpDataKey_Release( attrs_key );

    ISpRegDataKey_CreateKey( data_key, L"Voice2\\Attributes", &attrs_key );
    ISpDataKey_SetStringValue( attrs_key, L"Language", L"406;407;408;409;40a" );
    ISpDataKey_SetStringValue( attrs_key, L"Gender", L"Female" );
    ISpDataKey_SetStringValue( attrs_key, L"Age", L"Adult" );
    ISpDataKey_SetStringValue( attrs_key, L"Vendor", L"Vendor1" );
    ISpDataKey_Release( attrs_key );

    ISpRegDataKey_CreateKey( data_key, L"Voice3\\Attributes", &attrs_key );
    ISpDataKey_SetStringValue( attrs_key, L"Language", L"409;411" );
    ISpDataKey_SetStringValue( attrs_key, L"Gender", L"Female" );
    ISpDataKey_SetStringValue( attrs_key, L"Age", L"Child" );
    ISpDataKey_SetStringValue( attrs_key, L"Vendor", L"Vendor1" );
    ISpDataKey_Release( attrs_key );

    ISpRegDataKey_CreateKey( data_key, L"Voice4\\Attributes", &attrs_key );
    ISpDataKey_SetStringValue( attrs_key, L"Language", L"411" );
    ISpDataKey_SetStringValue( attrs_key, L"Gender", L"Male" );
    ISpDataKey_SetStringValue( attrs_key, L"Age", L"Adult" );
    ISpDataKey_Release( attrs_key );

    ISpRegDataKey_CreateKey( data_key, L"Voice5\\Attributes", &attrs_key );
    ISpDataKey_SetStringValue( attrs_key, L"Language", L"411" );
    ISpDataKey_SetStringValue( attrs_key, L"Gender", L"Female" );
    ISpDataKey_SetStringValue( attrs_key, L"Age", L"Adult" );
    ISpDataKey_SetStringValue( attrs_key, L"Vendor", L"Vendor2" );
    ISpDataKey_Release( attrs_key );

    ISpRegDataKey_Release( data_key );
}

static const WCHAR test_cat[] = L"HKEY_CURRENT_USER\\Software\\Winetest\\sapi\\TestVoices";

static void test_token_category(void)
{
    ISpObjectTokenCategory *cat;
    IEnumSpObjectTokens *enum_tokens;
    HRESULT hr;
    ULONG count;
    int i;
    ISpObjectToken *token;
    WCHAR *token_id;
    WCHAR tmp[MAX_PATH];

    hr = CoCreateInstance( &CLSID_SpObjectTokenCategory, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectTokenCategory, (void **)&cat );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpObjectTokenCategory_EnumTokens( cat, NULL, NULL, &enum_tokens );
    ok( hr == SPERR_UNINITIALIZED, "got %08lx\n", hr );

    hr = ISpObjectTokenCategory_SetId( cat, L"bogus", FALSE );
    ok( hr == SPERR_INVALID_REGISTRY_KEY, "got %08lx\n", hr );

    hr = ISpObjectTokenCategory_SetId( cat, test_cat, FALSE );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpObjectTokenCategory_SetId( cat, test_cat, FALSE );
    ok( hr == SPERR_ALREADY_INITIALIZED, "got %08lx\n", hr );

    hr = ISpObjectTokenCategory_EnumTokens( cat, NULL, NULL, &enum_tokens );
    ok( hr == S_OK, "got %08lx\n", hr );

    count = 0xdeadbeef;
    hr = IEnumSpObjectTokens_GetCount( enum_tokens, &count );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( count == 5, "got %lu\n", count );

    IEnumSpObjectTokens_Release( enum_tokens );

    hr = ISpObjectTokenCategory_EnumTokens( cat, L"", NULL, &enum_tokens );
    ok( hr == S_OK, "got %08lx\n", hr );

    count = 0xdeadbeef;
    hr = IEnumSpObjectTokens_GetCount( enum_tokens, &count );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( count == 5, "got %lu\n", count );

    IEnumSpObjectTokens_Release( enum_tokens );

    hr = ISpObjectTokenCategory_EnumTokens( cat, L"Language=409", NULL, &enum_tokens );
    ok( hr == S_OK, "got %08lx\n", hr );

    count = 0xdeadbeef;
    hr = IEnumSpObjectTokens_GetCount( enum_tokens, &count );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( count == 3, "got %lu\n", count );

    IEnumSpObjectTokens_Release( enum_tokens );

    hr = ISpObjectTokenCategory_EnumTokens( cat, L"Language=409", L"Vendor=Vendor1;Age=Child;Gender=Female",
                                            &enum_tokens );
    ok( hr == S_OK, "got %08lx\n", hr );

    count = 0xdeadbeef;
    hr = IEnumSpObjectTokens_GetCount( enum_tokens, &count );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( count == 3, "got %lu\n", count );

    for ( i = 0; i < 3; i++ ) {
        token = NULL;
        hr = IEnumSpObjectTokens_Item( enum_tokens, i, &token );
        ok( hr == S_OK, "i = %d: got %08lx\n", i, hr );

        token_id = NULL;
        hr = ISpObjectToken_GetId( token, &token_id );
        ok( hr == S_OK, "i = %d: got %08lx\n", i, hr );
        swprintf( tmp, MAX_PATH, L"%ls\\Tokens\\Voice%d", test_cat, 3 - i );
        ok( !wcscmp( token_id, tmp ), "i = %d: got %s\n", i, wine_dbgstr_w(token_id) );

        CoTaskMemFree( token_id );
        ISpObjectToken_Release( token );
    }

    IEnumSpObjectTokens_Release( enum_tokens );

    ISpObjectTokenCategory_Release( cat );
}

static void test_token_enum(void)
{
    ISpObjectTokenEnumBuilder *token_enum;
    HRESULT hr;
    IDispatch *disp;
    ISpeechObjectTokens *speech_tokens;
    IUnknown *unk;
    IEnumVARIANT *enumvar;
    ISpObjectToken *tokens[5];
    ISpObjectToken *out_tokens[5];
    WCHAR token_id[MAX_PATH];
    ULONG count;
    VARIANT vars[3], ret;
    DISPPARAMS params;
    int i;

    hr = CoCreateInstance( &CLSID_SpObjectTokenEnum, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectTokenEnumBuilder, (void **)&token_enum );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpObjectTokenEnumBuilder_QueryInterface( token_enum,
                                                   &IID_IDispatch, (void **)&disp );
    ok( hr == S_OK, "got %08lx\n", hr );
    IDispatch_Release( disp );

    hr = ISpObjectTokenEnumBuilder_QueryInterface( token_enum,
                                                   &IID_ISpeechObjectTokens,
                                                   (void **)&speech_tokens );
    ok( hr == S_OK, "got %08lx\n", hr );
    ISpeechObjectTokens_Release( speech_tokens );

    ISpObjectTokenEnumBuilder_Release( token_enum );

    hr = CoCreateInstance( &CLSID_SpObjectTokenEnum, NULL, CLSCTX_INPROC_SERVER,
                           &IID_IDispatch, (void **)&disp );
    ok( hr == S_OK, "got %08lx\n", hr );
    IDispatch_Release( disp );

    hr = CoCreateInstance( &CLSID_SpObjectTokenEnum, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpeechObjectTokens, (void **)&speech_tokens );
    ok( hr == S_OK, "got %08lx\n", hr );
    ISpeechObjectTokens_Release( speech_tokens );


    hr = CoCreateInstance( &CLSID_SpObjectTokenEnum, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectTokenEnumBuilder, (void **)&token_enum );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpObjectTokenEnumBuilder_GetCount( token_enum, &count );
    ok( hr == SPERR_UNINITIALIZED, "got %08lx\n", hr );

    hr = ISpObjectTokenEnumBuilder_Next( token_enum, 1, tokens, &count );
    ok( hr == SPERR_UNINITIALIZED, "got %08lx\n", hr );

    hr = ISpObjectTokenEnumBuilder_SetAttribs( token_enum, NULL, NULL );
    ok( hr == S_OK, "got %08lx\n", hr );

    count = 0xdeadbeef;
    hr = ISpObjectTokenEnumBuilder_GetCount( token_enum, &count );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( count == 0, "got %lu\n", count );

    count = 0xdeadbeef;
    hr = ISpObjectTokenEnumBuilder_Next( token_enum, 1, &out_tokens[0], &count );
    ok( hr == S_FALSE, "got %08lx\n", hr );
    ok( count == 0, "got %lu\n", count );

    for ( i = 0; i < 5; i++ ) {
        hr = CoCreateInstance( &CLSID_SpObjectToken, NULL, CLSCTX_INPROC_SERVER,
                               &IID_ISpObjectToken, (void **)&tokens[i] );
        ok( hr == S_OK, "got %08lx\n", hr );

        swprintf( token_id, MAX_PATH, L"%ls\\Tokens\\Voice%d", test_cat, i + 1 );
        hr = ISpObjectToken_SetId( tokens[i], NULL, token_id, FALSE );
        ok( hr == S_OK, "got %08lx\n", hr );
    }

    hr = ISpObjectTokenEnumBuilder_AddTokens( token_enum, 3, tokens );
    ok( hr == S_OK, "got %08lx\n", hr );

    out_tokens[0] = (ISpObjectToken *)0xdeadbeef;
    hr = ISpObjectTokenEnumBuilder_Next( token_enum, 1, &out_tokens[0], NULL );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( out_tokens[0] == tokens[0], "got %p\n", out_tokens[0] );

    out_tokens[0] = (ISpObjectToken *)0xdeadbeef;
    hr = ISpObjectTokenEnumBuilder_Item( token_enum, 0, &out_tokens[0] );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( out_tokens[0] == tokens[0], "got %p\n", out_tokens[0] );

    hr = ISpObjectTokenEnumBuilder_Item( token_enum, 3, &out_tokens[0] );
    ok( hr == SPERR_NO_MORE_ITEMS, "got %08lx\n", hr );

    hr = ISpObjectTokenEnumBuilder_Next( token_enum, 3, &out_tokens[1], NULL );
    ok( hr == E_POINTER, "got %08lx\n", hr );

    count = 0xdeadbeef;
    out_tokens[1] = out_tokens[2] = (ISpObjectToken *)0xdeadbeef;
    hr = ISpObjectTokenEnumBuilder_Next( token_enum, 3, &out_tokens[1], &count );
    ok( hr == S_FALSE, "got %08lx\n", hr );
    ok( count == 2, "got %lu\n", count );
    ok( out_tokens[1] == tokens[1], "got %p\n", out_tokens[1] );
    ok( out_tokens[2] == tokens[2], "got %p\n", out_tokens[2] );

    ISpObjectTokenEnumBuilder_Release( token_enum );

    hr = CoCreateInstance( &CLSID_SpObjectTokenEnum, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectTokenEnumBuilder, (void **)&token_enum );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpObjectTokenEnumBuilder_QueryInterface( token_enum,
                                                   &IID_ISpeechObjectTokens,
                                                   (void **)&speech_tokens );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpObjectTokenEnumBuilder_SetAttribs( token_enum, NULL, NULL );
    ok( hr == S_OK, "got %08lx\n", hr );
    hr = ISpObjectTokenEnumBuilder_AddTokens( token_enum, 3, tokens );
    ok( hr == S_OK, "got %08lx\n", hr );

    count = 0xdeadbeef;
    hr = ISpeechObjectTokens_get_Count( speech_tokens, (LONG *)&count );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( count == 3, "got %lu\n", count );

    hr = ISpeechObjectTokens_get__NewEnum( speech_tokens, &unk );
    ok( hr == S_OK, "got %08lx\n", hr );
    hr = IUnknown_QueryInterface( unk, &IID_IEnumVARIANT, (void **)&enumvar );
    ok( hr == S_OK, "got %08lx\n", hr );
    IUnknown_Release( unk );

    V_VT( &vars[0] ) = VT_ILLEGAL;
    V_DISPATCH( &vars[0] ) = (IDispatch *)0xdeadbeef;
    hr = IEnumVARIANT_Next( enumvar, 1, vars, NULL );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( V_VT( &vars[0] ) == VT_DISPATCH, "got %#x\n", V_VT( &vars[0] ) );
    ok( V_DISPATCH( &vars[0] ) != (IDispatch *)0xdeadbeef && V_DISPATCH( &vars[0] ) != NULL,
        "got %p\n", V_DISPATCH( &vars[0] ) );
    VariantClear( &vars[0] );

    for ( i = 0; i < 3; i++ ) {
        V_VT( &vars[i] ) = VT_ILLEGAL;
        V_DISPATCH( &vars[i] ) = (IDispatch *)0xdeadbeef;
    }
    count = 0xdeadbeef;

    hr = IEnumVARIANT_Next( enumvar, 3, vars, &count );
    ok( hr == S_FALSE, "got %08lx\n", hr );
    ok( count == 2, "got %lu\n", count );
    for ( i = 0; i < 2; i++ ) {
        ok( V_VT( &vars[i] ) == VT_DISPATCH, "got %#x\n", V_VT( &vars[i] ) );
        ok( V_DISPATCH( &vars[i] ) != (IDispatch *)0xdeadbeef && V_DISPATCH( &vars[i] ) != NULL,
            "got %p\n", V_DISPATCH( &vars[i] ) );
        VariantClear( &vars[i] );
    }
    ok( V_VT( &vars[2] ) == VT_ILLEGAL, "got %#x\n", V_VT( &vars[2] ) );

    IEnumVARIANT_Release( enumvar );

    memset( &params, 0, sizeof(params) );
    VariantInit( &ret );
    hr = ISpeechObjectTokens_Invoke( speech_tokens, DISPID_SOTsCount, &IID_NULL, 0,
                                     DISPATCH_PROPERTYGET, &params, &ret, NULL, NULL );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( V_VT( &ret ) == VT_I4, "got %#x\n", V_VT( &ret ) );
    ok( V_I4( &ret ) == 3, "got %ld\n", V_I4( &ret ) );

    ISpeechObjectTokens_Release( speech_tokens );
    ISpObjectTokenEnumBuilder_Release( token_enum );

    hr = CoCreateInstance( &CLSID_SpObjectTokenEnum, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectTokenEnumBuilder, (void **)&token_enum );
    ok( hr == S_OK, "got %08lx\n", hr );

    /* Vendor attribute must exist */
    hr = ISpObjectTokenEnumBuilder_SetAttribs( token_enum, L"Vendor", NULL );
    ok( hr == S_OK, "got %08lx\n", hr );
    hr = ISpObjectTokenEnumBuilder_AddTokens( token_enum, 5, tokens );
    ok( hr == S_OK, "got %08lx\n", hr );

    count = 0xdeadbeef;
    hr = ISpObjectTokenEnumBuilder_Next( token_enum, 5, &out_tokens[0], &count );
    ok( hr == S_FALSE, "got %08lx\n", hr );
    ok( count == 4, "got %lu\n", count );

    ISpObjectTokenEnumBuilder_Release( token_enum );

    hr = CoCreateInstance( &CLSID_SpObjectTokenEnum, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectTokenEnumBuilder, (void **)&token_enum );
    ok( hr == S_OK, "got %08lx\n", hr );

    /* Vendor attribute must contain Vendor1 */
    hr = ISpObjectTokenEnumBuilder_SetAttribs( token_enum, L"Vendor=Vendor1", NULL );
    ok( hr == S_OK, "got %08lx\n", hr );
    hr = ISpObjectTokenEnumBuilder_AddTokens( token_enum, 5, tokens );
    ok( hr == S_OK, "got %08lx\n", hr );

    count = 0xdeadbeef;
    hr = ISpObjectTokenEnumBuilder_Next( token_enum, 5, &out_tokens[0], &count );
    ok( hr == S_FALSE, "got %08lx\n", hr );
    ok( count == 2, "got %lu\n", count );

    ISpObjectTokenEnumBuilder_Release( token_enum );

    hr = CoCreateInstance( &CLSID_SpObjectTokenEnum, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectTokenEnumBuilder, (void **)&token_enum );
    ok( hr == S_OK, "got %08lx\n", hr );

    /* Vendor attribute must not contain Vendor1 */
    hr = ISpObjectTokenEnumBuilder_SetAttribs( token_enum, L"Vendor!=Vendor1", NULL );
    ok( hr == S_OK, "got %08lx\n", hr );
    hr = ISpObjectTokenEnumBuilder_AddTokens( token_enum, 5, tokens );
    ok( hr == S_OK, "got %08lx\n", hr );

    count = 0xdeadbeef;
    hr = ISpObjectTokenEnumBuilder_Next( token_enum, 5, &out_tokens[0], &count );
    ok( hr == S_FALSE, "got %08lx\n", hr );
    ok( count == 3, "got %lu\n", count );

    ISpObjectTokenEnumBuilder_Release( token_enum );

    hr = CoCreateInstance( &CLSID_SpObjectTokenEnum, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectTokenEnumBuilder, (void **)&token_enum );
    ok( hr == S_OK, "got %08lx\n", hr );

    /* Vendor attribute must contain Vendor1 and Language attribute must contain 407 */
    hr = ISpObjectTokenEnumBuilder_SetAttribs( token_enum, L"Vendor=Vendor1;Language=407", NULL );
    ok( hr == S_OK, "got %08lx\n", hr );
    hr = ISpObjectTokenEnumBuilder_AddTokens( token_enum, 5, tokens );
    ok( hr == S_OK, "got %08lx\n", hr );

    count = 0xdeadbeef;
    hr = ISpObjectTokenEnumBuilder_Next( token_enum, 5, &out_tokens[0], &count );
    ok( hr == S_FALSE, "got %08lx\n", hr );
    ok( count == 1, "got %lu\n", count );

    hr = CoCreateInstance( &CLSID_SpObjectTokenEnum, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectTokenEnumBuilder, (void **)&token_enum );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpObjectTokenEnumBuilder_SetAttribs( token_enum, L"Language=409",
                                               L"Vendor=Vendor1;Age=Child;Gender=Female" );
    ok( hr == S_OK, "got %08lx\n", hr );
    hr = ISpObjectTokenEnumBuilder_AddTokens( token_enum, 5, tokens );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpObjectTokenEnumBuilder_Sort( token_enum, NULL );
    ok( hr == S_OK, "got %08lx\n", hr );

    count = 0xdeadbeef;
    hr = ISpObjectTokenEnumBuilder_Next( token_enum, 5, &out_tokens[0], &count );
    ok( hr == S_FALSE, "got %08lx\n", hr );
    ok( count == 3, "got %lu\n", count );
    ok( out_tokens[0] == tokens[2], "got %p\n", out_tokens[0] );
    ok( out_tokens[1] == tokens[1], "got %p\n", out_tokens[1] );
    ok( out_tokens[2] == tokens[0], "got %p\n", out_tokens[2] );

    ISpObjectTokenEnumBuilder_Release( token_enum );

    for ( i = 0; i < 5; i++ ) ISpObjectToken_Release( tokens[i] );
}

static void test_default_token_id(void)
{
    ISpObjectTokenCategory *cat;
    HRESULT hr;
    LPWSTR token_id = NULL;
    LONG res;
    WCHAR regvalue[512];
    DWORD regvalue_size;

    hr = CoCreateInstance( &CLSID_SpObjectTokenCategory, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectTokenCategory, (void **)&cat );
    ok( hr == S_OK, "got %08lx\n", hr );

    token_id = (LPWSTR)0xdeadbeef;
    hr = ISpObjectTokenCategory_GetDefaultTokenId( cat, &token_id );
    ok( hr == SPERR_UNINITIALIZED, "got %08lx\n", hr );
    ok( token_id == (LPWSTR)0xdeadbeef, "got %p\n", token_id );

    hr = ISpObjectTokenCategory_GetDefaultTokenId( cat, NULL );
    ok( hr == SPERR_UNINITIALIZED, "got %08lx\n", hr );

    hr = ISpObjectTokenCategory_SetId( cat, SPCAT_AUDIOOUT, FALSE );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpObjectTokenCategory_GetDefaultTokenId( cat, NULL );
    ok( hr == E_POINTER, "got %08lx\n", hr );

    token_id = (LPWSTR)0xdeadbeef;
    hr = ISpObjectTokenCategory_GetDefaultTokenId( cat, &token_id );

    /* AudioOutput under windows server returns this error */
    if (hr == SPERR_NOT_FOUND) {
        /* also happens if TokenEnums/Tokens is empty or doesn't exist */
        skip( "AudioOutput category not found for GetDefaultTokenId\n" );
        return;
    }

    ok( hr == S_OK, "got %08lx\n", hr );
    ok( token_id != (LPWSTR)0xdeadbeef && token_id != NULL, "got %p\n", token_id );

    regvalue_size = sizeof( regvalue );
    res = RegGetValueW( HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Speech\\AudioOutput",
                        L"DefaultDefaultTokenId", RRF_RT_REG_SZ, NULL,
                        (LPVOID)&regvalue, &regvalue_size);
    if (res == ERROR_FILE_NOT_FOUND) {
        skip( "DefaultDefaultTokenId not found for AudioOutput category (%s)\n",
              wine_dbgstr_w(token_id) );
    } else {
        ok( res == ERROR_SUCCESS, "got %08lx\n", res );
        ok( !wcscmp(regvalue, token_id),
            "GetDefaultTokenId (%s) should be equal to the DefaultDefaultTokenId key (%s)\n",
            wine_dbgstr_w(token_id), wine_dbgstr_w(regvalue) );
    }

    CoTaskMemFree( token_id );
    ISpObjectTokenCategory_Release( cat );
}

static const WCHAR default_cat_key[] = L"Software\\Winetest\\sapi\\DefaultChoice";

/* Recreates the category HKEY_CURRENT_USER\Software\Winetest\sapi\DefaultChoice with tokens
 * named "A", "B", ... and the given Language attributes (NULL: no Attributes key). */
static ISpObjectTokenCategory *create_default_category( const WCHAR **langs, unsigned int count )
{
    ISpObjectTokenCategory *cat;
    WCHAR path[MAX_PATH];
    unsigned int i;
    LSTATUS ret;
    HRESULT hr;
    HKEY key;

    RegDeleteTreeW( HKEY_CURRENT_USER, default_cat_key );
    swprintf( path, ARRAY_SIZE(path), L"%ls\\Tokens", default_cat_key );
    ret = RegCreateKeyExW( HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL );
    ok( ret == ERROR_SUCCESS, "got %ld\n", ret );
    RegCloseKey( key );

    for (i = 0; i < count; i++)
    {
        if (langs[i])
            swprintf( path, ARRAY_SIZE(path), L"%ls\\Tokens\\%c\\Attributes", default_cat_key, 'A' + i );
        else
            swprintf( path, ARRAY_SIZE(path), L"%ls\\Tokens\\%c", default_cat_key, 'A' + i );
        ret = RegCreateKeyExW( HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL );
        ok( ret == ERROR_SUCCESS, "got %ld\n", ret );
        if (langs[i])
            RegSetValueExW( key, L"Language", 0, REG_SZ, (const BYTE *)langs[i],
                            (wcslen( langs[i] ) + 1) * sizeof(WCHAR) );
        RegCloseKey( key );
    }

    hr = CoCreateInstance( &CLSID_SpObjectTokenCategory, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectTokenCategory, (void **)&cat );
    ok( hr == S_OK, "got %08lx\n", hr );
    swprintf( path, ARRAY_SIZE(path), L"HKEY_CURRENT_USER\\%ls", default_cat_key );
    hr = ISpObjectTokenCategory_SetId( cat, path, FALSE );
    ok( hr == S_OK, "got %08lx\n", hr );
    return cat;
}

/* Checks the DefaultTokenId stored in the category key: the token name, or NULL for none. */
#define check_stored_default(a) check_stored_default_(__LINE__, a)
static void check_stored_default_( unsigned int line, const WCHAR *name )
{
    WCHAR value[MAX_PATH] = { 0 }, expect[MAX_PATH];
    DWORD size = sizeof(value);
    LSTATUS ret;

    ret = RegGetValueW( HKEY_CURRENT_USER, default_cat_key, L"DefaultTokenId", RRF_RT_REG_SZ,
                        NULL, value, &size );
    if (!name)
    {
        ok_(__FILE__, line)( ret == ERROR_FILE_NOT_FOUND, "got %ld, %s\n", ret, wine_dbgstr_w(value) );
        return;
    }
    swprintf( expect, ARRAY_SIZE(expect), L"HKEY_CURRENT_USER\\%ls\\Tokens\\%ls", default_cat_key, name );
    ok_(__FILE__, line)( ret == ERROR_SUCCESS, "got %ld\n", ret );
    ok_(__FILE__, line)( !wcscmp( value, expect ), "got %s\n", wine_dbgstr_w(value) );
}

#define check_default(a, b, c) check_default_(__LINE__, a, b, c)
static void check_default_( unsigned int line, ISpObjectTokenCategory *cat, HRESULT expect_hr,
                            const WCHAR *name )
{
    WCHAR expect[MAX_PATH], *id = NULL;
    HRESULT hr;

    hr = ISpObjectTokenCategory_GetDefaultTokenId( cat, &id );
    ok_(__FILE__, line)( hr == expect_hr, "got %08lx\n", hr );
    if (hr != S_OK) return;
    swprintf( expect, ARRAY_SIZE(expect), L"HKEY_CURRENT_USER\\%ls\\Tokens\\%ls", default_cat_key, name );
    ok_(__FILE__, line)( !wcscmp( id, expect ), "got %s\n", wine_dbgstr_w(id) );
    CoTaskMemFree( id );
}

static void test_default_token_choice(void)
{
    WCHAR ui[16], primary[16], other[16], list[32], path[MAX_PATH];
    LANGID lang = GetUserDefaultUILanguage();
    IEnumSpObjectTokens *enum_tokens;
    ISpObjectTokenCategory *cat;
    const WCHAR *langs[3];
    HRESULT hr;

    /* the user's UI language, the same primary language with another sublanguage, and
     * another language */
    swprintf( ui, ARRAY_SIZE(ui), L"%x", lang );
    swprintf( primary, ARRAY_SIZE(primary), L"%x",
              MAKELANGID( PRIMARYLANGID(lang), SUBLANGID(lang) == SUBLANG_DEFAULT ? 2 : SUBLANG_DEFAULT ) );
    swprintf( other, ARRAY_SIZE(other), L"%x", PRIMARYLANGID(lang) == LANG_FRENCH ? 0x410 : 0x40c );
    swprintf( list, ARRAY_SIZE(list), L"%ls;%ls", other, ui );

    /* Without a default, the first token of the UI language becomes the default and is
     * stored in the category key. */
    langs[0] = other; langs[1] = ui; langs[2] = ui;
    cat = create_default_category( langs, 3 );
    check_stored_default( NULL );
    check_default( cat, S_OK, L"B" );
    check_stored_default( L"B" );
    ISpObjectTokenCategory_Release( cat );

    /* A token that only shares the primary language does not count, the first token wins. */
    langs[0] = other; langs[1] = primary;
    cat = create_default_category( langs, 2 );
    check_default( cat, S_OK, L"A" );
    check_stored_default( L"A" );
    ISpObjectTokenCategory_Release( cat );

    langs[0] = other; langs[1] = list;
    cat = create_default_category( langs, 2 );
    check_default( cat, S_OK, L"B" );
    ISpObjectTokenCategory_Release( cat );

    langs[0] = NULL; langs[1] = ui;
    cat = create_default_category( langs, 2 );
    check_default( cat, S_OK, L"B" );
    ISpObjectTokenCategory_Release( cat );

    cat = create_default_category( langs, 0 );
    check_default( cat, SPERR_NOT_FOUND, NULL );
    check_stored_default( NULL );
    ISpObjectTokenCategory_Release( cat );

    /* EnumTokens() stores the default as well. */
    langs[0] = other; langs[1] = ui;
    cat = create_default_category( langs, 2 );
    hr = ISpObjectTokenCategory_EnumTokens( cat, NULL, NULL, &enum_tokens );
    ok( hr == S_OK, "got %08lx\n", hr );
    IEnumSpObjectTokens_Release( enum_tokens );
    check_stored_default( L"B" );
    ISpObjectTokenCategory_Release( cat );

    /* SetDefaultTokenId() stores any id, even one without a token. */
    cat = create_default_category( langs, 2 );
    swprintf( path, ARRAY_SIZE(path), L"HKEY_CURRENT_USER\\%ls\\Tokens\\A", default_cat_key );
    hr = ISpObjectTokenCategory_SetDefaultTokenId( cat, path );
    ok( hr == S_OK, "got %08lx\n", hr );
    check_stored_default( L"A" );
    check_default( cat, S_OK, L"A" );
    swprintf( path, ARRAY_SIZE(path), L"HKEY_CURRENT_USER\\%ls\\Tokens\\Missing", default_cat_key );
    hr = ISpObjectTokenCategory_SetDefaultTokenId( cat, path );
    ok( hr == S_OK, "got %08lx\n", hr );
    check_stored_default( L"Missing" );
    hr = ISpObjectTokenCategory_SetDefaultTokenId( cat, NULL );
    ok( hr == E_INVALIDARG, "got %08lx\n", hr );
    ISpObjectTokenCategory_Release( cat );

    RegDeleteTreeW( HKEY_CURRENT_USER, default_cat_key );
}

static void tests_token_voices(void)
{
    ISpObjectTokenCategory *cat;
    HRESULT hr;
    IEnumSpObjectTokens *tokens;
    ISpObjectToken *item;
    ULONG fetched = 0;
    ULONG count;

    hr = CoCreateInstance( &CLSID_SpObjectTokenCategory, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectTokenCategory, (void **)&cat );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpObjectTokenCategory_SetId( cat, SPCAT_VOICES, FALSE );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpObjectTokenCategory_EnumTokens(cat, NULL, NULL, &tokens);
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = IEnumSpObjectTokens_GetCount( tokens, &count );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( count != 0, "got %lu\n", count );
    ISpObjectTokenCategory_Release( cat );

    hr = IEnumSpObjectTokens_Item(tokens, 0, &item);
    ok( hr == S_OK, "got %08lx\n", hr );
    ISpObjectToken_Release(item);

    hr = IEnumSpObjectTokens_Next(tokens, 1, &item, &fetched);
    ok( hr == S_OK, "got %08lx\n", hr );
    ISpObjectToken_Release(item);

    IEnumSpObjectTokens_Release(tokens);
}


#define TESTCLASS_CLSID L"{67DD26B6-50BA-3297-253E-619346F177F8}"
static const GUID CLSID_TestClass = {0x67DD26B6,0x50BA,0x3297,{0x25,0x3E,0x61,0x93,0x46,0xF1,0x77,0xF8}};

static ISpObjectToken *test_class_token;

static HRESULT WINAPI test_class_QueryInterface(ISpObjectWithToken *iface, REFIID riid, void **ppv)
{
    if (IsEqualGUID( riid, &IID_IUnknown ) || IsEqualGUID( riid, &IID_ISpObjectWithToken ))
    {
        *ppv = iface;
        return S_OK;
    }

    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI test_class_AddRef(ISpObjectWithToken *iface)
{
    return 2;
}

static ULONG WINAPI test_class_Release(ISpObjectWithToken *iface)
{
    return 1;
}

static HRESULT WINAPI test_class_SetObjectToken(ISpObjectWithToken *iface, ISpObjectToken *token)
{
    ok( token != NULL, "token == NULL\n" );
    test_class_token = token;
    ISpObjectToken_AddRef(test_class_token);
    return S_OK;
}

static HRESULT WINAPI test_class_GetObjectToken(ISpObjectWithToken *iface, ISpObjectToken **token)
{
    ok( 0, "unexpected call\n" );
    return E_NOTIMPL;
}

static const ISpObjectWithTokenVtbl test_class_vtbl = {
    test_class_QueryInterface,
    test_class_AddRef,
    test_class_Release,
    test_class_SetObjectToken,
    test_class_GetObjectToken
};

static ISpObjectWithToken test_class = { &test_class_vtbl };

static HRESULT WINAPI ClassFactory_QueryInterface(IClassFactory *iface, REFIID riid, void **ppv)
{
    if (IsEqualGUID( &IID_IUnknown, riid ) || IsEqualGUID( &IID_IClassFactory, riid ))
    {
        *ppv = iface;
        return S_OK;
    }

    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI ClassFactory_AddRef(IClassFactory *iface)
{
    return 2;
}

static ULONG WINAPI ClassFactory_Release(IClassFactory *iface)
{
    return 1;
}

static HRESULT WINAPI ClassFactory_CreateInstance(IClassFactory *iface,
        IUnknown *pUnkOuter, REFIID riid, void **ppv)
{
    ok( pUnkOuter == NULL, "pUnkOuter != NULL\n" );
    ok( IsEqualGUID(riid, &IID_IUnknown), "riid = %s\n", wine_dbgstr_guid(riid) );

    *ppv = &test_class;
    return S_OK;
}

static HRESULT WINAPI ClassFactory_LockServer(IClassFactory *iface, BOOL fLock)
{
    ok( 0, "unexpected call\n" );
    return E_NOTIMPL;
}

static const IClassFactoryVtbl ClassFactoryVtbl = {
    ClassFactory_QueryInterface,
    ClassFactory_AddRef,
    ClassFactory_Release,
    ClassFactory_CreateInstance,
    ClassFactory_LockServer
};

static IClassFactory test_class_cf = { &ClassFactoryVtbl };

static void test_object_token(void)
{
    static const WCHAR test_token_id[] = L"HKEY_LOCAL_MACHINE\\Software\\Winetest\\sapi\\TestToken";
    static const WCHAR *get_description = L"GetDescription";

    ISpObjectToken *token;
    IDispatch *disp;
    ISpeechObjectToken *speech_token;
    ISpDataKey *sub_key;
    HRESULT hr;
    LPWSTR tempW, token_id;
    BSTR tempB;
    ISpObjectTokenCategory *cat;
    DWORD regid;
    IUnknown *obj;
    DISPID dispid;
    DISPPARAMS params;
    VARIANT arg, ret;

    hr = CoCreateInstance( &CLSID_SpObjectToken, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectToken, (void **)&token );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpObjectToken_QueryInterface( token, &IID_IDispatch, (void **)&disp );
    ok( hr == S_OK, "got %08lx\n", hr );
    IDispatch_Release( disp );

    hr = ISpObjectToken_QueryInterface( token, &IID_ISpeechObjectToken, (void **)&speech_token );
    ok( hr == S_OK, "got %08lx\n", hr );
    ISpeechObjectToken_Release( speech_token );

    ISpObjectToken_Release( token );

    hr = CoCreateInstance( &CLSID_SpObjectToken, NULL, CLSCTX_INPROC_SERVER,
                           &IID_IDispatch, (void **)&disp );
    ok( hr == S_OK, "got %08lx\n", hr );
    IDispatch_Release( disp );

    hr = CoCreateInstance( &CLSID_SpObjectToken, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpeechObjectToken, (void **)&speech_token );
    ok( hr == S_OK, "got %08lx\n", hr );
    ISpeechObjectToken_Release( speech_token );


    hr = CoCreateInstance( &CLSID_SpObjectToken, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectToken, (void **)&token );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpObjectToken_GetId( token, NULL );
    ok( hr == SPERR_UNINITIALIZED, "got %08lx\n", hr );

    tempW = (LPWSTR)0xdeadbeef;
    hr = ISpObjectToken_GetId( token, &tempW );
    ok( hr == SPERR_UNINITIALIZED, "got %08lx\n", hr );
    ok( tempW == (LPWSTR)0xdeadbeef, "got %s\n", wine_dbgstr_w(tempW) );

    hr = ISpObjectToken_GetCategory( token, NULL );
    todo_wine ok( hr == SPERR_UNINITIALIZED, "got %08lx\n", hr );

    cat = (LPVOID)0xdeadbeef;
    hr = ISpObjectToken_GetCategory( token, &cat );
    todo_wine ok( hr == SPERR_UNINITIALIZED, "got %08lx\n", hr );
    ok( cat == (LPVOID)0xdeadbeef, "got %p\n", cat );

    hr = ISpObjectToken_SetId( token, NULL, NULL, FALSE );
    ok( hr == E_POINTER, "got %08lx\n", hr );
    hr = ISpObjectToken_SetId( token, L"bogus", NULL, FALSE );
    ok( hr == E_POINTER, "got %08lx\n", hr );

    hr = ISpObjectToken_SetId( token, NULL, L"bogus", FALSE );
    ok( hr == SPERR_NOT_FOUND, "got %08lx\n", hr );
    hr = ISpObjectToken_SetId( token, NULL, L"HKEY_LOCAL_MACHINE\\SOFTWARE\\winetest bogus", FALSE );
    ok( hr == SPERR_NOT_FOUND, "got %08lx\n", hr );

    /* SetId succeeds even if the key is invalid, but exists */
    hr = ISpObjectToken_SetId( token, NULL, L"HKEY_LOCAL_MACHINE\\SOFTWARE", FALSE );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpObjectToken_SetId( token, NULL, NULL, FALSE );
    ok( hr == SPERR_ALREADY_INITIALIZED, "got %08lx\n", hr );
    hr = ISpObjectToken_SetId( token, NULL, L"bogus", FALSE );
    ok( hr == SPERR_ALREADY_INITIALIZED, "got %08lx\n", hr );

    hr = ISpObjectToken_GetId( token, NULL );
    ok( hr == E_POINTER, "got %08lx\n", hr );

    hr = ISpObjectToken_GetCategory( token, NULL );
    todo_wine ok( hr == E_POINTER, "got %08lx\n", hr );

    tempW = NULL;
    hr = ISpObjectToken_GetId( token, &tempW );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( tempW != NULL, "got %p\n", tempW );
    if (tempW) {
        ok( !wcscmp(tempW, L"HKEY_LOCAL_MACHINE\\SOFTWARE"), "got %s\n",
            wine_dbgstr_w(tempW) );
        CoTaskMemFree( tempW );
    }

    hr = ISpObjectToken_OpenKey(token, L"Non-exist", &sub_key);
    ok( hr == SPERR_NOT_FOUND, "got %08lx\n", hr );

    hr = ISpObjectToken_OpenKey(token, L"Classes", &sub_key);
    ok( hr == S_OK, "got %08lx\n", hr );
    ISpDataKey_Release(sub_key);

    cat = (LPVOID)0xdeadbeef;
    hr = ISpObjectToken_GetCategory( token, &cat );
    todo_wine ok( hr == SPERR_INVALID_REGISTRY_KEY, "got %08lx\n", hr );
    ok( cat == (LPVOID)0xdeadbeef, "got %p\n", cat );

    /* get the default token id for SPCAT_AUDIOOUT */
    hr = CoCreateInstance( &CLSID_SpObjectTokenCategory, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectTokenCategory, (void **)&cat );
    ok( hr == S_OK, "got %08lx\n", hr );
    hr = ISpObjectTokenCategory_SetId( cat, SPCAT_AUDIOOUT, FALSE );
    ok( hr == S_OK, "got %08lx\n", hr );
    token_id = (LPWSTR)0xdeadbeef;
    hr = ISpObjectTokenCategory_GetDefaultTokenId( cat, &token_id );
    if (hr == SPERR_NOT_FOUND) {
        skip( "AudioOutput category not found for GetDefaultTokenId\n" );
        return;
    }
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( token_id != (LPWSTR)0xdeadbeef && token_id != NULL, "got %p\n", token_id );
    ISpObjectTokenCategory_Release( cat );

    /* recreate token in order to SetId again */
    ISpObjectToken_Release( token );
    hr = CoCreateInstance( &CLSID_SpObjectToken, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectToken, (void **)&token );
    ok( hr == S_OK, "got %08lx\n", hr );

    /* NULL appears to auto-detect the category */
    hr = ISpObjectToken_SetId( token, NULL, token_id, FALSE );
    ok( hr == S_OK, "got %08lx\n", hr );

    tempW = NULL;
    hr = ISpObjectToken_GetId( token, &tempW );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( tempW != NULL, "got %p\n", tempW );
    if (tempW) {
        ok( !wcsncmp(tempW, token_id, wcslen(token_id)),
            "got %s (expected %s)\n", wine_dbgstr_w(tempW), wine_dbgstr_w(token_id) );
        CoTaskMemFree( tempW );
    }

    cat = (LPVOID)0xdeadbeef;
    hr = ISpObjectToken_GetCategory( token, &cat );
    todo_wine ok( hr == S_OK, "got %08lx\n", hr );
    todo_wine ok( cat != (LPVOID)0xdeadbeef, "got %p\n", cat );
    if (cat != (LPVOID)0xdeadbeef) {
        tempW = NULL;
        hr = ISpObjectTokenCategory_GetId( cat, &tempW );
        todo_wine ok( hr == S_OK, "got %08lx\n", hr );
        todo_wine ok( tempW != NULL, "got %p\n", tempW );
        if (tempW) {
            ok( !wcscmp(tempW, SPCAT_AUDIOOUT), "got %s\n", wine_dbgstr_w(tempW) );
            CoTaskMemFree( tempW );
        }

        /* not freed by ISpObjectToken_Release */
        ISpObjectTokenCategory_Release( cat );
    }

    hr = CoRegisterClassObject( &CLSID_TestClass, (IUnknown *)&test_class_cf,
                                CLSCTX_INPROC_SERVER, REGCLS_MULTIPLEUSE, &regid );
    ok( hr == S_OK, "got %08lx\n", hr );

    ISpObjectToken_Release( token );
    hr = CoCreateInstance( &CLSID_SpObjectToken, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectToken, (void **)&token );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpObjectToken_SetId( token, NULL, test_token_id, TRUE );
    ok( hr == S_OK || broken(hr == E_ACCESSDENIED) /* win1064_adm */, "got %08lx\n", hr );
    if (hr == E_ACCESSDENIED) {
        win_skip( "token SetId access denied\n" );
        ISpObjectToken_Release( token );
        return;
    }

    hr = ISpObjectToken_CreateKey( token, L"Attributes", &sub_key );
    ok( hr == S_OK, "got %08lx\n", hr );
    ISpDataKey_Release( sub_key );

    hr = ISpObjectToken_SetStringValue( token, L"CLSID", TESTCLASS_CLSID );
    ok( hr == S_OK, "got %08lx\n", hr );

    tempW = NULL;
    hr = ISpObjectToken_GetStringValue( token, L"CLSID", &tempW );

    ok( hr == S_OK, "got %08lx\n", hr );
    if ( tempW ) {
        ok( !wcsncmp( tempW, TESTCLASS_CLSID, wcslen(TESTCLASS_CLSID) ),
            "got %s (expected %s)\n", wine_dbgstr_w(tempW), wine_dbgstr_w(TESTCLASS_CLSID) );
        CoTaskMemFree( tempW );
    }

    test_class_token = NULL;
    hr = ISpObjectToken_CreateInstance( token, NULL, CLSCTX_INPROC_SERVER, &IID_IUnknown, (void **)&obj );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( test_class_token != NULL, "test_class_token not set\n" );

    tempW = NULL;
    hr = ISpObjectToken_GetId( test_class_token, &tempW );
    ok( tempW != NULL, "got %p\n", tempW );
    if (tempW) {
        ok( !wcsncmp(tempW, test_token_id, wcslen(test_token_id)),
            "got %s (expected %s)\n", wine_dbgstr_w(tempW), wine_dbgstr_w(test_token_id) );
        CoTaskMemFree( tempW );
    }

    hr = ISpObjectToken_SetStringValue( token, L"409", L"409 - TestToken" );
    ok( hr == S_OK, "got %08lx\n", hr );
    hr = ISpObjectToken_SetStringValue( token, L"407", L"407 - TestToken" );
    ok( hr == S_OK, "got %08lx\n", hr );
    hr = ISpObjectToken_SetStringValue( token, L"E40C", L"E40C - TestToken" );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpObjectToken_QueryInterface( token, &IID_ISpeechObjectToken, (void **)&speech_token );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpeechObjectToken_GetDescription( speech_token, 0x409, NULL );
    ok( hr == E_POINTER, "got %08lx\n", hr );

    tempB = NULL;
    hr = ISpeechObjectToken_GetDescription( speech_token, 0x409, &tempB );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( tempB && !wcscmp( tempB, L"409 - TestToken" ), "got %s\n", wine_dbgstr_w( tempB ) );
    SysFreeString( tempB );

    tempB = NULL;
    hr = ISpeechObjectToken_GetDescription( speech_token, 0x10407, &tempB );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( tempB && !wcscmp( tempB, L"407 - TestToken" ), "got %s\n", wine_dbgstr_w( tempB ) );
    SysFreeString( tempB );

    tempB = NULL;
    hr = ISpeechObjectToken_GetDescription( speech_token, 0xE40C, &tempB );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( tempB && !wcscmp( tempB, L"E40C - TestToken" ), "got %s\n", wine_dbgstr_w( tempB ) );
    SysFreeString( tempB );

    tempB = (BSTR)0xdeadbeef;
    hr = ISpeechObjectToken_GetDescription( speech_token, 0x406, &tempB );
    ok( hr == SPERR_NOT_FOUND, "got %08lx\n", hr );
    ok( tempB == (BSTR)0xdeadbeef || broken(tempB == NULL) /* < win7 */, "got %p\n", tempB );

    hr = ISpObjectToken_SetStringValue( token, NULL, L"TestToken" );
    ok( hr == S_OK, "got %08lx\n", hr );

    tempB = NULL;
    hr = ISpeechObjectToken_GetDescription( speech_token, 0x406, &tempB );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( tempB && !wcscmp( tempB, L"TestToken" ), "got %s\n", wine_dbgstr_w( tempB ) );
    SysFreeString( tempB );

    tempB = NULL;
    hr = ISpeechObjectToken_GetDescription( speech_token, 0x0, &tempB );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( tempB && !wcscmp( tempB, L"TestToken" ), "got %s\n", wine_dbgstr_w( tempB ) );
    SysFreeString( tempB );

    dispid = 0xdeadbeef;
    hr = ISpeechObjectToken_GetIDsOfNames( speech_token, &IID_NULL, (WCHAR **)&get_description, 1, 0x409, &dispid );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( dispid == DISPID_SOTGetDescription, "got %08lx\n", dispid );

    memset( &params, 0, sizeof(params) );
    params.cArgs = 1;
    params.cNamedArgs = 0;
    params.rgvarg = &arg;
    VariantInit( &arg );
    V_VT( &arg ) = VT_I4;
    V_I4( &arg ) = 0x409;
    VariantInit( &ret );
    hr = ISpeechObjectToken_Invoke( speech_token, DISPID_SOTGetDescription, &IID_NULL,
                                    0, DISPATCH_METHOD, &params, &ret, NULL, NULL );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( V_VT( &ret ) == VT_BSTR, "got %#x\n", V_VT( &ret ) );
    ok( V_BSTR( &ret ) && !wcscmp( V_BSTR( &ret ), L"409 - TestToken" ),
        "got %s\n", wine_dbgstr_w( V_BSTR( &ret ) ) );
    VariantClear( &ret );

    ISpeechObjectToken_Release( speech_token );

    ISpObjectToken_Release( test_class_token );
    IUnknown_Release( obj );
    ISpObjectToken_Release( token );

    RegDeleteTreeA( HKEY_LOCAL_MACHINE, "Software\\Winetest\\sapi" );
}

static const WCHAR storage_token_id[] = L"HKEY_CURRENT_USER\\Software\\Winetest\\sapi\\StorageToken";

static ISpObjectToken *create_storage_token( ISpDataKey **attrs )
{
    ISpObjectToken *token;
    HRESULT hr;

    hr = CoCreateInstance( &CLSID_SpObjectToken, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectToken, (void **)&token );
    ok( hr == S_OK, "got %08lx\n", hr );
    hr = ISpObjectToken_SetId( token, NULL, storage_token_id, TRUE );
    ok( hr == S_OK, "got %08lx\n", hr );
    hr = ISpObjectToken_CreateKey( token, L"Attributes", attrs );
    ok( hr == S_OK, "got %08lx\n", hr );
    hr = ISpDataKey_SetStringValue( *attrs, L"Language", L"409;9" );
    ok( hr == S_OK, "got %08lx\n", hr );
    hr = ISpDataKey_SetStringValue( *attrs, L"Gender", L"Female" );
    ok( hr == S_OK, "got %08lx\n", hr );
    return token;
}

static void test_object_token_init(void)
{
    ISpObjectTokenInit *init;
    ISpObjectToken *token, *token2;
    ISpDataKey *attrs;
    WCHAR *str;
    HRESULT hr;

    hr = CoCreateInstance( &CLSID_SpObjectToken, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectToken, (void **)&token );
    ok( hr == S_OK, "got %08lx\n", hr );
    hr = ISpObjectToken_QueryInterface( token, &IID_ISpObjectTokenInit, (void **)&init );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( (void *)init == (void *)token, "got %p, %p\n", init, token );
    ISpObjectTokenInit_Release( init );
    ISpObjectToken_Release( token );

    token = create_storage_token( &attrs );

    hr = CoCreateInstance( &CLSID_SpObjectToken, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectTokenInit, (void **)&init );
    ok( hr == S_OK, "got %08lx\n", hr );

    hr = ISpObjectTokenInit_InitFromDataKey( init, NULL, NULL, attrs );
    ok( hr == E_POINTER, "got %08lx\n", hr );
    hr = ISpObjectTokenInit_InitFromDataKey( init, NULL, L"TestToken", NULL );
    ok( hr == E_POINTER, "got %08lx\n", hr );
    hr = ISpObjectTokenInit_InitFromDataKey( init, NULL, L"TestToken", attrs );
    ok( hr == S_OK, "got %08lx\n", hr );

    str = NULL;
    hr = ISpObjectTokenInit_GetId( init, &str );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( str && !wcscmp( str, L"TestToken" ), "got %s\n", wine_dbgstr_w(str) );
    CoTaskMemFree( str );

    str = NULL;
    hr = ISpObjectTokenInit_GetStringValue( init, L"Gender", &str );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( str && !wcscmp( str, L"Female" ), "got %s\n", wine_dbgstr_w(str) );
    CoTaskMemFree( str );

    hr = ISpObjectTokenInit_InitFromDataKey( init, NULL, L"TestToken2", attrs );
    ok( hr == SPERR_ALREADY_INITIALIZED, "got %08lx\n", hr );
    hr = ISpObjectTokenInit_SetId( init, NULL, storage_token_id, FALSE );
    ok( hr == SPERR_ALREADY_INITIALIZED, "got %08lx\n", hr );

    hr = ISpObjectTokenInit_QueryInterface( init, &IID_ISpObjectToken, (void **)&token2 );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( (void *)token2 == (void *)init, "got %p, %p\n", token2, init );
    ISpObjectToken_Release( token2 );
    ISpObjectTokenInit_Release( init );

    hr = CoCreateInstance( &CLSID_SpObjectToken, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectTokenInit, (void **)&init );
    ok( hr == S_OK, "got %08lx\n", hr );
    hr = ISpObjectTokenInit_SetId( init, NULL, storage_token_id, FALSE );
    ok( hr == S_OK, "got %08lx\n", hr );
    hr = ISpObjectTokenInit_InitFromDataKey( init, NULL, L"TestToken", attrs );
    ok( hr == SPERR_ALREADY_INITIALIZED, "got %08lx\n", hr );
    ISpObjectTokenInit_Release( init );

    ISpDataKey_Release( attrs );
    ISpObjectToken_Release( token );
    RegDeleteTreeA( HKEY_CURRENT_USER, "Software\\Winetest\\sapi" );
}

static void test_object_token_matches(void)
{
    static const struct
    {
        const WCHAR *attrs;
        BOOL match;
    }
    tests[] =
    {
        { L"Language=409", TRUE },
        { L"Language=9", TRUE },
        { L"Language=407", FALSE },
        { L"Gender=Female;Language=409", TRUE },
        { L"Gender=Male;Language=409", FALSE },
        { L"Gender", TRUE },
        { L"Age", FALSE },
        { L"Age!=Adult", TRUE },
        { L"Gender!=Female", FALSE },
        { L"Gender!=Male", TRUE },
        { L"gender=female", TRUE },
        { L"", TRUE },
        { NULL, TRUE },
    };
    ISpObjectToken *token;
    ISpDataKey *attrs;
    unsigned int i;
    BOOL match;
    HRESULT hr;

    hr = CoCreateInstance( &CLSID_SpObjectToken, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectToken, (void **)&token );
    ok( hr == S_OK, "got %08lx\n", hr );
    match = 0xdeadbeef;
    hr = ISpObjectToken_MatchesAttributes( token, L"Language=409", &match );
    ok( hr == SPERR_UNINITIALIZED, "got %08lx\n", hr );
    ok( match == 0xdeadbeef, "got %d\n", match );
    ISpObjectToken_Release( token );

    token = create_storage_token( &attrs );

    for (i = 0; i < ARRAY_SIZE(tests); i++)
    {
        match = !tests[i].match;
        hr = ISpObjectToken_MatchesAttributes( token, tests[i].attrs, &match );
        ok( hr == S_OK, "%u: got %08lx\n", i, hr );
        ok( match == tests[i].match, "%u: got %d\n", i, match );
    }

    hr = ISpObjectToken_MatchesAttributes( token, L"Language=409", NULL );
    ok( hr == E_INVALIDARG, "got %08lx\n", hr );

    ISpDataKey_Release( attrs );
    ISpObjectToken_Release( token );
    RegDeleteTreeA( HKEY_CURRENT_USER, "Software\\Winetest\\sapi" );
}

static void test_object_token_storage(void)
{
    static const WCHAR files_key[] = L"Software\\Winetest\\sapi\\StorageToken\\" TESTCLASS_CLSID L"\\Files";
    static const WCHAR stored_prefix[] = L"%1a%\\Microsoft\\Speech\\Files\\Winetest\\SP_";
    WCHAR appdata[MAX_PATH], dir[MAX_PATH], expect[MAX_PATH], buf[MAX_PATH], *path, *path2;
    ISpObjectToken *token;
    ISpDataKey *attrs;
    DWORD size;
    HRESULT hr;
    LONG res;

    hr = CoCreateInstance( &CLSID_SpObjectToken, NULL, CLSCTX_INPROC_SERVER,
                           &IID_ISpObjectToken, (void **)&token );
    ok( hr == S_OK, "got %08lx\n", hr );
    hr = ISpObjectToken_GetStorageFileName( token, &CLSID_TestClass, L"Datafile", L"Winetest\\",
                                            CSIDL_APPDATA | CSIDL_FLAG_CREATE, &path );
    ok( hr == SPERR_UNINITIALIZED, "got %08lx\n", hr );
    ISpObjectToken_Release( token );

    token = create_storage_token( &attrs );

    hr = ISpObjectToken_GetStorageFileName( token, &CLSID_TestClass, NULL, L"Winetest\\",
                                            CSIDL_APPDATA | CSIDL_FLAG_CREATE, &path );
    ok( hr == E_INVALIDARG, "got %08lx\n", hr );
    hr = ISpObjectToken_GetStorageFileName( token, &CLSID_TestClass, L"Datafile", L"Winetest\\",
                                            CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL );
    ok( hr == E_POINTER, "got %08lx\n", hr );

    /* Without a folder, nothing is created. */
    hr = ISpObjectToken_GetStorageFileName( token, &CLSID_TestClass, L"Datafile", NULL, 0, &path );
    ok( hr == SPERR_NOT_FOUND, "got %08lx\n", hr );
    hr = ISpObjectToken_GetStorageFileName( token, &CLSID_TestClass, L"Datafile", L"C:\\winetest-sapi\\", 0, &path );
    ok( hr == SPERR_NOT_FOUND, "got %08lx\n", hr );

    SHGetFolderPathW( NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appdata );
    swprintf( dir, ARRAY_SIZE(dir), L"%s\\Microsoft\\Speech\\Files\\Winetest", appdata );
    swprintf( expect, ARRAY_SIZE(expect), L"%s\\SP_", dir );

    /* A new file gets a generated name and S_FALSE. */
    path = NULL;
    hr = ISpObjectToken_GetStorageFileName( token, &CLSID_TestClass, L"Datafile", L"Winetest\\",
                                            CSIDL_APPDATA | CSIDL_FLAG_CREATE, &path );
    ok( hr == S_FALSE, "got %08lx\n", hr );
    ok( path && !wcsncmp( path, expect, wcslen( expect ) ) && wcslen( path ) == wcslen( expect ) + 36 &&
        !wcscmp( path + wcslen( path ) - 4, L".dat" ), "got %s\n", wine_dbgstr_w(path) );
    ok( GetFileAttributesW( path ) != INVALID_FILE_ATTRIBUTES, "file %s not created\n", wine_dbgstr_w(path) );

    size = sizeof(buf);
    res = RegGetValueW( HKEY_CURRENT_USER, files_key, L"Datafile", RRF_RT_REG_SZ, NULL, buf, &size );
    ok( !res, "got %ld\n", res );
    ok( !wcsncmp( buf, stored_prefix, wcslen( stored_prefix ) ) &&
        !wcscmp( buf + wcslen( stored_prefix ), path + wcslen( expect ) ), "got %s\n", wine_dbgstr_w(buf) );

    /* From now on the stored file is returned, whatever the other arguments are. */
    path2 = NULL;
    hr = ISpObjectToken_GetStorageFileName( token, &CLSID_TestClass, L"Datafile", L"Other\\",
                                            CSIDL_LOCAL_APPDATA, &path2 );
    ok( hr == S_OK, "got %08lx\n", hr );
    ok( path2 && !wcscmp( path, path2 ), "got %s, expected %s\n", wine_dbgstr_w(path2), wine_dbgstr_w(path) );
    CoTaskMemFree( path2 );

    path2 = NULL;
    hr = ISpObjectToken_GetStorageFileName( token, &CLSID_TestClass, L"Named", L"Winetest\\named.dat",
                                            CSIDL_APPDATA | CSIDL_FLAG_CREATE, &path2 );
    ok( hr == S_FALSE, "got %08lx\n", hr );
    swprintf( expect, ARRAY_SIZE(expect), L"%s\\named.dat", dir );
    ok( path2 && !wcscmp( path2, expect ), "got %s\n", wine_dbgstr_w(path2) );
    ok( GetFileAttributesW( path2 ) != INVALID_FILE_ATTRIBUTES, "file %s not created\n", wine_dbgstr_w(path2) );
    size = sizeof(buf);
    res = RegGetValueW( HKEY_CURRENT_USER, files_key, L"Named", RRF_RT_REG_SZ, NULL, buf, &size );
    ok( !res, "got %ld\n", res );
    ok( !wcscmp( buf, L"%1a%\\Microsoft\\Speech\\Files\\Winetest\\named.dat" ), "got %s\n", wine_dbgstr_w(buf) );

    DeleteFileW( path2 );
    CoTaskMemFree( path2 );
    DeleteFileW( path );
    CoTaskMemFree( path );
    RemoveDirectoryW( dir );

    ISpDataKey_Release( attrs );
    ISpObjectToken_Release( token );
    RegDeleteTreeA( HKEY_CURRENT_USER, "Software\\Winetest\\sapi" );
}

START_TEST(token)
{
    CoInitialize( NULL );
    RegDeleteTreeA( HKEY_CURRENT_USER, "Software\\Winetest\\sapi" );
    test_data_key();
    setup_test_voice_tokens();
    test_token_category();
    test_token_enum();
    test_default_token_id();
    test_default_token_choice();
    test_object_token();
    test_object_token_init();
    test_object_token_matches();
    test_object_token_storage();
    tests_token_voices();
    CoUninitialize();
}
