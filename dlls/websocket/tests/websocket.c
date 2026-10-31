/*
 * WebSocket Protocol Component API tests
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
#include <stdio.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "wincrypt.h"
#include "websocket.h"

#include "wine/test.h"

#define RECV_SIZE 256
#define SEND_SIZE 256
#define OVERHEAD  144

static DECLSPEC_ALIGN(8) BYTE alloc_buf[RECV_SIZE + SEND_SIZE + OVERHEAD + 64];
static BYTE *alloc_ptr = alloc_buf;
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
static BYTE app_buf[2048];

static WEB_SOCKET_HTTP_HEADER hdr( const char *name, const char *value )
{
    WEB_SOCKET_HTTP_HEADER h;
    h.pcName = (char *)name;
    h.ulNameLength = strlen( name );
    h.pcValue = (char *)value;
    h.ulValueLength = strlen( value );
    return h;
}

static const char *action_name( WEB_SOCKET_ACTION a )
{
    switch (a)
    {
    case WEB_SOCKET_NO_ACTION: return "NO_ACTION";
    case WEB_SOCKET_SEND_TO_NETWORK_ACTION: return "SEND_TO_NETWORK";
    case WEB_SOCKET_INDICATE_SEND_COMPLETE_ACTION: return "INDICATE_SEND_COMPLETE";
    case WEB_SOCKET_RECEIVE_FROM_NETWORK_ACTION: return "RECEIVE_FROM_NETWORK";
    case WEB_SOCKET_INDICATE_RECEIVE_COMPLETE_ACTION: return "INDICATE_RECEIVE_COMPLETE";
    }
    return "???";
}

static const char *type_name( WEB_SOCKET_BUFFER_TYPE t )
{
    switch (t)
    {
    case WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE: return "UTF8_MESSAGE";
    case WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE: return "UTF8_FRAGMENT";
    case WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE: return "BINARY_MESSAGE";
    case WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE: return "BINARY_FRAGMENT";
    case WEB_SOCKET_CLOSE_BUFFER_TYPE: return "CLOSE";
    case WEB_SOCKET_PING_PONG_BUFFER_TYPE: return "PING_PONG";
    case WEB_SOCKET_UNSOLICITED_PONG_BUFFER_TYPE: return "UNSOLICITED_PONG";
    default: return "???";
    }
}

static const char *where( const BYTE *p, ULONG len, char *buf )
{
    if (!p) return "null";
    if (p >= alloc_buf && p + len <= alloc_buf + sizeof(alloc_buf))
        sprintf( buf, "alloc+%u", (unsigned)(p - alloc_buf) );
    else if (p >= app_buf && p + len <= app_buf + sizeof(app_buf))
        sprintf( buf, "app+%u", (unsigned)(p - app_buf) );
    else
        sprintf( buf, "other(%p)", p );
    return buf;
}

static void dump_bytes( const char *label, const BYTE *p, ULONG len )
{
    char line[200];
    ULONG i, n = min( len, 40 );
    int pos = 0;
    line[0] = 0;
    for (i = 0; i < n; i++) pos += sprintf( line + pos, "%02x", p[i] );
    trace( "    %s len %lu: %s%s\n", label, len, line, len > n ? "..." : "" );
}

/* one WebSocketGetAction with full dump; returns the action */
static WEB_SOCKET_ACTION get_action( WEB_SOCKET_HANDLE h, WEB_SOCKET_ACTION_QUEUE queue, ULONG count,
                                     WEB_SOCKET_BUFFER *bufs, ULONG *out_count, WEB_SOCKET_BUFFER_TYPE *out_type,
                                     void **app_ctx, void **act_ctx, HRESULT *out_hr )
{
    WEB_SOCKET_ACTION action = 0xdead;
    WEB_SOCKET_BUFFER_TYPE type = 0xdead;
    void *actx = (void *)0xdead, *cctx = (void *)0xdead;
    ULONG i, n = count;
    char w[64];
    HRESULT hr;

    memset( bufs, 0xcc, 2 * sizeof(*bufs) );
    hr = WebSocketGetAction( h, queue, bufs, &n, &action, &type, &actx, &cctx );
    trace( "  GetAction(q=%u,in=%lu): hr %#lx action %s type %s(%#x) count %lu appctx %p actctx %s\n",
           queue, count, hr, action_name( action ), type_name( type ), type, n, actx, cctx ? "set" : "NULL" );
    if (SUCCEEDED(hr) && action != WEB_SOCKET_NO_ACTION)
    {
        for (i = 0; i < n; i++)
        {
            if (type == WEB_SOCKET_CLOSE_BUFFER_TYPE)
            {
                trace( "    buf[%lu] close: reason %s len %lu status %u\n", i,
                       where( bufs[i].CloseStatus.pbReason, bufs[i].CloseStatus.ulReasonLength, w ),
                       bufs[i].CloseStatus.ulReasonLength, bufs[i].CloseStatus.usStatus );
                if (bufs[i].CloseStatus.pbReason) dump_bytes( "reason", bufs[i].CloseStatus.pbReason, bufs[i].CloseStatus.ulReasonLength );
            }
            else
            {
                trace( "    buf[%lu] %s len %lu\n", i, where( bufs[i].Data.pbBuffer, bufs[i].Data.ulBufferLength, w ),
                       bufs[i].Data.ulBufferLength );
                if (bufs[i].Data.pbBuffer && action != WEB_SOCKET_RECEIVE_FROM_NETWORK_ACTION)
                    dump_bytes( "data", bufs[i].Data.pbBuffer, bufs[i].Data.ulBufferLength );
            }
        }
    }
    if (out_count) *out_count = n;
    if (out_type) *out_type = type;
    if (app_ctx) *app_ctx = actx;
    if (act_ctx) *act_ctx = cctx;
    if (out_hr) *out_hr = hr;
    return SUCCEEDED(hr) ? action : WEB_SOCKET_NO_ACTION;
}

static ULONG sum_len( const WEB_SOCKET_BUFFER *bufs, ULONG n )
{
    ULONG i, s = 0;
    for (i = 0; i < n; i++) s += bufs[i].Data.ulBufferLength;
    return s;
}

/* drive the queue until NO_ACTION: sends are "transmitted" in full, receives get fed from feed/feed_len */
static void run_queue( WEB_SOCKET_HANDLE h, WEB_SOCKET_ACTION_QUEUE queue, ULONG count,
                       const BYTE *feed, ULONG feed_len, int max_steps )
{
    WEB_SOCKET_BUFFER bufs[2];
    WEB_SOCKET_BUFFER_TYPE type;
    WEB_SOCKET_ACTION action;
    ULONG n, pos = 0;
    void *actx, *cctx;
    HRESULT hr;

    while (max_steps-- > 0)
    {
        action = get_action( h, queue, count, bufs, &n, &type, &actx, &cctx, &hr );
        if (FAILED(hr) || action == WEB_SOCKET_NO_ACTION) return;
        if (action == WEB_SOCKET_RECEIVE_FROM_NETWORK_ACTION)
        {
            ULONG chunk = min( bufs[0].Data.ulBufferLength, feed_len - pos );
            memcpy( bufs[0].Data.pbBuffer, feed + pos, chunk );
            pos += chunk;
            trace( "    -> fed %lu bytes (feed pos %lu/%lu)\n", chunk, pos, feed_len );
            WebSocketCompleteAction( h, cctx, chunk );
            if (!chunk) return;
        }
        else if (action == WEB_SOCKET_SEND_TO_NETWORK_ACTION)
            WebSocketCompleteAction( h, cctx, sum_len( bufs, n ) );
        else
            WebSocketCompleteAction( h, cctx, 0 );
    }
    trace( "  (step limit reached)\n" );
}

static void dump_headers( const char *label, const WEB_SOCKET_HTTP_HEADER *h, ULONG count )
{
    ULONG i;
    trace( "  %s: %lu headers\n", label, count );
    for (i = 0; i < count; i++)
        trace( "    [%lu] %.*s (%lu) = %.*s (%lu)\n", i, (int)h[i].ulNameLength, h[i].pcName, h[i].ulNameLength,
               (int)h[i].ulValueLength, h[i].pcValue, h[i].ulValueLength );
}

static const char *find_header( const WEB_SOCKET_HTTP_HEADER *h, ULONG count, const char *name, char *out )
{
    ULONG i;
    for (i = 0; i < count; i++)
    {
        if (h[i].ulNameLength == strlen( name ) && !_strnicmp( h[i].pcName, name, h[i].ulNameLength ))
        {
            memcpy( out, h[i].pcValue, h[i].ulValueLength );
            out[h[i].ulValueLength] = 0;
            return out;
        }
    }
    return NULL;
}

static void compute_accept( const char *key, char *out )
{
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    HCRYPTPROV prov;
    HCRYPTHASH hash;
    BYTE digest[20];
    DWORD len = 20, out_len = 64;
    char input[128];
    BOOL ret;

    strcpy( input, key );
    strcat( input, guid );
    ret = CryptAcquireContextW( &prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT );
    ok( ret, "CryptAcquireContext failed %lu\n", GetLastError() );
    ret = CryptCreateHash( prov, CALG_SHA1, 0, 0, &hash );
    ok( ret, "CryptCreateHash failed %lu\n", GetLastError() );
    ret = CryptHashData( hash, (BYTE *)input, strlen( input ), 0 );
    ok( ret, "CryptHashData failed %lu\n", GetLastError() );
    ret = CryptGetHashParam( hash, HP_HASHVAL, digest, &len, 0 );
    ok( ret, "CryptGetHashParam failed %lu\n", GetLastError() );
    CryptDestroyHash( hash );
    CryptReleaseContext( prov, 0 );
    ret = CryptBinaryToStringA( digest, 20, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out, &out_len );
    ok( ret, "CryptBinaryToString failed %lu\n", GetLastError() );
}

/* the .NET Framework dummy handshake: Begin with Connection/Upgrade, End with Connection/Upgrade/Accept */
static HRESULT fake_client_handshake( WEB_SOCKET_HANDLE h, BOOL verbose )
{
    WEB_SOCKET_HTTP_HEADER initial[2], response[3], *additional;
    ULONG count;
    char key[64], accept[64];
    HRESULT hr;

    initial[0] = hdr( "Connection", "Upgrade" );
    initial[1] = hdr( "Upgrade", "websocket" );
    hr = WebSocketBeginClientHandshake( h, NULL, 0, NULL, 0, initial, 2, &additional, &count );
    if (verbose) trace( "  BeginClientHandshake: hr %#lx\n", hr );
    if (FAILED(hr)) return hr;
    if (verbose) dump_headers( "additional", additional, count );
    if (!find_header( additional, count, "Sec-WebSocket-Key", key )) return E_FAIL;
    compute_accept( key, accept );
    response[0] = hdr( "Connection", "Upgrade" );
    response[1] = hdr( "Upgrade", "websocket" );
    response[2] = hdr( "Sec-WebSocket-Accept", accept );
    hr = WebSocketEndClientHandshake( h, response, 3, NULL, NULL, NULL );
    if (verbose) trace( "  EndClientHandshake: hr %#lx\n", hr );
    return hr;
}

static WEB_SOCKET_HANDLE create_client( BOOL use_alloc, BOOL disable_masking, ULONG recv_size, ULONG send_size, HRESULT *out_hr )
{
    WEB_SOCKET_PROPERTY props[4];
    ULONG count = 0, alloc_size = recv_size + send_size + OVERHEAD;
    BOOL disable = TRUE;
    WEB_SOCKET_HANDLE h = (WEB_SOCKET_HANDLE)0xdeadbeef;
    HRESULT hr;

    props[count].Type = WEB_SOCKET_RECEIVE_BUFFER_SIZE_PROPERTY_TYPE; props[count].pvValue = &recv_size; props[count++].ulValueSize = sizeof(recv_size);
    props[count].Type = WEB_SOCKET_SEND_BUFFER_SIZE_PROPERTY_TYPE; props[count].pvValue = &send_size; props[count++].ulValueSize = sizeof(send_size);
    if (use_alloc)
    {
        memset( alloc_buf, 0xaa, sizeof(alloc_buf) );
        props[count].Type = WEB_SOCKET_ALLOCATED_BUFFER_PROPERTY_TYPE; props[count].pvValue = &alloc_ptr; props[count++].ulValueSize = alloc_size;
    }
    if (disable_masking)
    {
        props[count].Type = WEB_SOCKET_DISABLE_MASKING_PROPERTY_TYPE; props[count].pvValue = &disable; props[count++].ulValueSize = sizeof(disable);
    }
    hr = WebSocketCreateClientHandle( props, count, &h );
    if (out_hr) *out_hr = hr;
    return SUCCEEDED(hr) ? h : NULL;
}

static ULONG build_frame( BYTE *out, BYTE opcode, BOOL fin, BYTE rsv, const void *payload, ULONG len, const BYTE *mask )
{
    ULONG i, pos = 2;
    out[0] = (fin ? 0x80 : 0) | (rsv << 4) | opcode;
    if (len < 126) out[1] = len;
    else { out[1] = 126; out[2] = len >> 8; out[3] = len & 0xff; pos = 4; }
    if (mask) { out[1] |= 0x80; memcpy( out + pos, mask, 4 ); pos += 4; }
    for (i = 0; i < len; i++) out[pos + i] = ((const BYTE *)payload)[i] ^ (mask ? mask[i % 4] : 0);
    return pos + len;
}

static void test_global_property(void)
{
    ULONG value = 0xdeadbeef, size, versions[4];
    HRESULT hr;

    trace( "=== global properties\n" );
    size = sizeof(value);
    hr = WebSocketGetGlobalProperty( WEB_SOCKET_KEEPALIVE_INTERVAL_PROPERTY_TYPE, &value, &size );
    trace( "  keepalive: hr %#lx value %lu size %lu\n", hr, value, size );
    size = 2;
    hr = WebSocketGetGlobalProperty( WEB_SOCKET_KEEPALIVE_INTERVAL_PROPERTY_TYPE, &value, &size );
    trace( "  keepalive size 2: hr %#lx size %lu\n", hr, size );
    size = 0;
    hr = WebSocketGetGlobalProperty( WEB_SOCKET_SUPPORTED_VERSIONS_PROPERTY_TYPE, NULL, &size );
    trace( "  versions size 0/NULL: hr %#lx size %lu\n", hr, size );
    memset( versions, 0xcc, sizeof(versions) );
    size = sizeof(versions);
    hr = WebSocketGetGlobalProperty( WEB_SOCKET_SUPPORTED_VERSIONS_PROPERTY_TYPE, versions, &size );
    trace( "  versions size 16: hr %#lx size %lu v0 %lu v1 %#lx\n", hr, size, versions[0], versions[1] );
    size = sizeof(value);
    hr = WebSocketGetGlobalProperty( WEB_SOCKET_RECEIVE_BUFFER_SIZE_PROPERTY_TYPE, &value, &size );
    trace( "  recv buffer size (not global): hr %#lx\n", hr );
    hr = WebSocketGetGlobalProperty( WEB_SOCKET_KEEPALIVE_INTERVAL_PROPERTY_TYPE, NULL, &size );
    trace( "  keepalive NULL value: hr %#lx\n", hr );
    hr = WebSocketGetGlobalProperty( WEB_SOCKET_KEEPALIVE_INTERVAL_PROPERTY_TYPE, &value, NULL );
    trace( "  keepalive NULL size: hr %#lx\n", hr );
}

static void test_create_handle(void)
{
    WEB_SOCKET_HANDLE h;
    WEB_SOCKET_PROPERTY prop;
    ULONG v;
    HRESULT hr;

    trace( "=== create handle\n" );
    h = (WEB_SOCKET_HANDLE)0xdeadbeef;
    hr = WebSocketCreateClientHandle( NULL, 0, &h );
    trace( "  client NULL props: hr %#lx handle %p\n", hr, h );
    if (SUCCEEDED(hr)) { WebSocketAbortHandle( h ); trace( "  abort fresh handle survived\n" ); WebSocketDeleteHandle( h ); trace( "  delete survived\n" ); }
    /* NULL output pointer and NULL properties with a count crash the native component */

    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    trace( "  client recv %u send %u alloc %u: hr %#lx\n", RECV_SIZE, SEND_SIZE, RECV_SIZE + SEND_SIZE + OVERHEAD, hr );
    if (h) WebSocketDeleteHandle( h );
    h = create_client( TRUE, FALSE, RECV_SIZE, 16, &hr );
    trace( "  client send 16: hr %#lx\n", hr );
    if (h) WebSocketDeleteHandle( h );
    h = create_client( TRUE, FALSE, 100, SEND_SIZE, &hr );
    trace( "  client recv 100: hr %#lx\n", hr );
    if (h) WebSocketDeleteHandle( h );
    h = create_client( FALSE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    trace( "  client without alloc: hr %#lx\n", hr );
    if (h) WebSocketDeleteHandle( h );

    v = RECV_SIZE;
    alloc_ptr = alloc_buf;
    prop.Type = WEB_SOCKET_ALLOCATED_BUFFER_PROPERTY_TYPE; prop.pvValue = &alloc_ptr; prop.ulValueSize = RECV_SIZE + SEND_SIZE;
    hr = WebSocketCreateClientHandle( &prop, 1, &h );
    trace( "  alloc only, size recv+send (no overhead), default sizes: hr %#lx\n", hr );
    if (SUCCEEDED(hr)) WebSocketDeleteHandle( h );
    prop.ulValueSize = 4096 + 4096 + OVERHEAD;
    hr = WebSocketCreateClientHandle( &prop, 1, &h );
    trace( "  alloc only, 4096+4096+144 (default sizes): hr %#lx\n", hr );
    if (SUCCEEDED(hr)) WebSocketDeleteHandle( h );
    prop.ulValueSize = 4096 + 4096 + 256;
    hr = WebSocketCreateClientHandle( &prop, 1, &h );
    trace( "  alloc only, 4096+4096+256: hr %#lx\n", hr );
    if (SUCCEEDED(hr)) WebSocketDeleteHandle( h );
    alloc_ptr = alloc_buf + 1; prop.ulValueSize = sizeof(alloc_buf) - 1;
    hr = WebSocketCreateClientHandle( &prop, 1, &h );
    trace( "  alloc unaligned: hr %#lx\n", hr );
    if (SUCCEEDED(hr)) WebSocketDeleteHandle( h );
    alloc_ptr = alloc_buf; prop.ulValueSize = 8;
    hr = WebSocketCreateClientHandle( &prop, 1, &h );
    trace( "  alloc 8 bytes (default sizes): hr %#lx\n", hr );
    if (SUCCEEDED(hr)) WebSocketDeleteHandle( h );
    prop.pvValue = alloc_buf; prop.ulValueSize = sizeof(alloc_buf);
    hr = WebSocketCreateClientHandle( &prop, 1, &h );
    trace( "  alloc pvValue = buffer itself (not pointer to it): hr %#lx\n", hr );
    if (SUCCEEDED(hr)) WebSocketDeleteHandle( h );
    alloc_ptr = alloc_buf;
    prop.Type = WEB_SOCKET_RECEIVE_BUFFER_SIZE_PROPERTY_TYPE; prop.pvValue = &v; prop.ulValueSize = 2;
    hr = WebSocketCreateClientHandle( &prop, 1, &h );
    trace( "  recv size with ulValueSize 2: hr %#lx\n", hr );
    if (SUCCEEDED(hr)) WebSocketDeleteHandle( h );
    prop.Type = WEB_SOCKET_KEEPALIVE_INTERVAL_PROPERTY_TYPE; prop.pvValue = &v; prop.ulValueSize = 4;
    hr = WebSocketCreateClientHandle( &prop, 1, &h );
    trace( "  keepalive as handle property: hr %#lx\n", hr );
    if (SUCCEEDED(hr)) WebSocketDeleteHandle( h );
    prop.Type = 77; prop.pvValue = &v; prop.ulValueSize = 4;
    hr = WebSocketCreateClientHandle( &prop, 1, &h );
    trace( "  unknown property type: hr %#lx\n", hr );
    if (SUCCEEDED(hr)) WebSocketDeleteHandle( h );

    hr = WebSocketCreateServerHandle( NULL, 0, &h );
    trace( "  server NULL props: hr %#lx handle %p\n", hr, h );
    if (SUCCEEDED(hr)) WebSocketDeleteHandle( h );

    /* a NULL handle makes the native component report an exception (RtlReportException), not tested */
}

static void test_client_handshake(void)
{
    WEB_SOCKET_HTTP_HEADER initial[3], response[6], *additional;
    const char *subprotocols[2] = { "chat", "superchat" };
    const char *extensions[2] = { "permessage-deflate", "x-foo" };
    ULONG count, sel_ext[4], sel_ext_count, sel_proto;
    WEB_SOCKET_HANDLE h;
    WEB_SOCKET_BUFFER buf;
    char key[64], accept[64];
    HRESULT hr;

    trace( "=== client handshake\n" );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    if (!h) return;

    /* session functions before handshake */
    buf.Data.pbBuffer = app_buf; buf.Data.ulBufferLength = 5;
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &buf, NULL );
    trace( "  Send before handshake: hr %#lx\n", hr );
    hr = WebSocketReceive( h, NULL, NULL );
    trace( "  Receive before handshake: hr %#lx\n", hr );
    hr = WebSocketEndClientHandshake( h, NULL, 0, NULL, NULL, NULL );
    trace( "  End before Begin: hr %#lx\n", hr );

    initial[0] = hdr( "Connection", "Upgrade" );
    initial[1] = hdr( "Upgrade", "websocket" );
    initial[2] = hdr( "Host", "example.com" );
    hr = WebSocketBeginClientHandshake( h, (PCSTR *)subprotocols, 2, (PCSTR *)extensions, 2, initial, 3, &additional, &count );
    trace( "  Begin with 2 subprotocols, 2 extensions, 3 initial: hr %#lx\n", hr );
    if (SUCCEEDED(hr)) dump_headers( "additional", additional, count );
    hr = WebSocketBeginClientHandshake( h, NULL, 0, NULL, 0, initial, 2, &additional, &count );
    trace( "  Begin twice: hr %#lx\n", hr );
    find_header( additional, count, "Sec-WebSocket-Key", key );
    compute_accept( key, accept );
    trace( "  key %s (len %u) accept %s\n", key, (unsigned)strlen( key ), accept );

    /* server picks the second subprotocol and the first extension */
    response[0] = hdr( "Connection", "Upgrade" );
    response[1] = hdr( "Upgrade", "websocket" );
    response[2] = hdr( "Sec-WebSocket-Accept", accept );
    response[3] = hdr( "Sec-WebSocket-Protocol", "superchat" );
    response[4] = hdr( "Sec-WebSocket-Extensions", "permessage-deflate" );
    memset( sel_ext, 0xcc, sizeof(sel_ext) ); sel_ext_count = 4; sel_proto = 0xcccccccc;
    hr = WebSocketEndClientHandshake( h, response, 5, sel_ext, &sel_ext_count, &sel_proto );
    trace( "  End (proto superchat, ext deflate): hr %#lx sel_proto %lu sel_ext_count %lu sel_ext[0] %lu\n", hr, sel_proto, sel_ext_count, sel_ext[0] );
    hr = WebSocketEndClientHandshake( h, response, 5, sel_ext, &sel_ext_count, &sel_proto );
    trace( "  End twice: hr %#lx\n", hr );
    hr = WebSocketReceive( h, NULL, NULL );
    trace( "  Receive after handshake: hr %#lx\n", hr );
    WebSocketDeleteHandle( h );

    /* negatives, each on a fresh handle */
#define BEGIN_PLAIN() do { h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr ); \
        hr = WebSocketBeginClientHandshake( h, (PCSTR *)subprotocols, 2, (PCSTR *)extensions, 2, initial, 2, &additional, &count ); \
        find_header( additional, count, "Sec-WebSocket-Key", key ); compute_accept( key, accept ); \
        response[0] = hdr( "Connection", "Upgrade" ); response[1] = hdr( "Upgrade", "websocket" ); \
        response[2] = hdr( "Sec-WebSocket-Accept", accept ); } while (0)

    BEGIN_PLAIN();
    response[2] = hdr( "Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" );
    hr = WebSocketEndClientHandshake( h, response, 3, NULL, NULL, NULL );
    trace( "  End wrong accept: hr %#lx\n", hr );
    hr = WebSocketEndClientHandshake( h, response, 3, NULL, NULL, NULL );
    trace( "  End again after failure: hr %#lx\n", hr );
    WebSocketDeleteHandle( h );

    BEGIN_PLAIN();
    hr = WebSocketEndClientHandshake( h, response + 2, 1, NULL, NULL, NULL );
    trace( "  End accept only (no Upgrade/Connection): hr %#lx\n", hr );
    WebSocketDeleteHandle( h );

    BEGIN_PLAIN();
    response[0] = hdr( "connection", "keep-alive, upgrade" );
    response[1] = hdr( "UPGRADE", "WebSocket" );
    hr = WebSocketEndClientHandshake( h, response, 3, NULL, NULL, NULL );
    trace( "  End odd case / connection list: hr %#lx\n", hr );
    WebSocketDeleteHandle( h );

    BEGIN_PLAIN();
    response[3] = hdr( "Sec-WebSocket-Protocol", "nope" );
    sel_proto = 0xcccccccc;
    hr = WebSocketEndClientHandshake( h, response, 4, NULL, NULL, &sel_proto );
    trace( "  End unknown protocol: hr %#lx sel_proto %#lx\n", hr, sel_proto );
    WebSocketDeleteHandle( h );

    BEGIN_PLAIN();
    sel_proto = 0xcccccccc;
    hr = WebSocketEndClientHandshake( h, response, 3, NULL, NULL, &sel_proto );
    trace( "  End no protocol chosen (2 offered): hr %#lx sel_proto %#lx\n", hr, sel_proto );
    WebSocketDeleteHandle( h );

    BEGIN_PLAIN();
    response[3] = hdr( "Sec-WebSocket-Extensions", "x-bar" );
    sel_ext_count = 4;
    hr = WebSocketEndClientHandshake( h, response, 4, sel_ext, &sel_ext_count, NULL );
    trace( "  End unknown extension: hr %#lx sel_ext_count %lu\n", hr, sel_ext_count );
    WebSocketDeleteHandle( h );

    BEGIN_PLAIN();
    response[3] = hdr( "Sec-WebSocket-Extensions", "x-foo, permessage-deflate" );
    memset( sel_ext, 0xcc, sizeof(sel_ext) ); sel_ext_count = 4;
    hr = WebSocketEndClientHandshake( h, response, 4, sel_ext, &sel_ext_count, NULL );
    trace( "  End two extensions: hr %#lx sel_ext_count %lu [0]=%lu [1]=%lu\n", hr, sel_ext_count, sel_ext[0], sel_ext[1] );
    WebSocketDeleteHandle( h );

    BEGIN_PLAIN();
    response[3] = hdr( "Sec-WebSocket-Extensions", "x-foo, permessage-deflate" );
    sel_ext_count = 1;
    hr = WebSocketEndClientHandshake( h, response, 4, sel_ext, &sel_ext_count, NULL );
    trace( "  End two extensions, room for 1: hr %#lx sel_ext_count %lu\n", hr, sel_ext_count );
    WebSocketDeleteHandle( h );

    BEGIN_PLAIN();
    response[3] = hdr( "Sec-WebSocket-Extensions", "x-foo" );
    hr = WebSocketEndClientHandshake( h, response, 4, NULL, NULL, NULL );
    trace( "  End extension chosen, NULL out arrays: hr %#lx\n", hr );
    WebSocketDeleteHandle( h );

    BEGIN_PLAIN();
    hr = WebSocketEndClientHandshake( h, NULL, 0, NULL, NULL, NULL );
    trace( "  End no headers: hr %#lx\n", hr );
    WebSocketDeleteHandle( h );
#undef BEGIN_PLAIN

    /* a client handle without subprotocols/extensions but a Sec-WebSocket-Protocol in the response */
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    hr = WebSocketBeginClientHandshake( h, NULL, 0, NULL, 0, initial, 2, &additional, &count );
    find_header( additional, count, "Sec-WebSocket-Key", key ); compute_accept( key, accept );
    response[0] = hdr( "Connection", "Upgrade" ); response[1] = hdr( "Upgrade", "websocket" );
    response[2] = hdr( "Sec-WebSocket-Accept", accept ); response[3] = hdr( "Sec-WebSocket-Protocol", "chat" );
    hr = WebSocketEndClientHandshake( h, response, 4, NULL, NULL, NULL );
    trace( "  End protocol offered by server but none requested: hr %#lx\n", hr );
    WebSocketDeleteHandle( h );

    /* initial headers without Connection/Upgrade */
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    hr = WebSocketBeginClientHandshake( h, NULL, 0, NULL, 0, NULL, 0, &additional, &count );
    trace( "  Begin with no initial headers: hr %#lx\n", hr );
    if (SUCCEEDED(hr)) dump_headers( "additional", additional, count );
    WebSocketDeleteHandle( h );

    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    initial[0] = hdr( "Sec-WebSocket-Key", "AAAAAAAAAAAAAAAAAAAAAA==" );
    initial[1] = hdr( "Sec-WebSocket-Version", "8" );
    hr = WebSocketBeginClientHandshake( h, NULL, 0, NULL, 0, initial, 2, &additional, &count );
    trace( "  Begin with own Key/Version in initial headers: hr %#lx\n", hr );
    if (SUCCEEDED(hr)) dump_headers( "additional", additional, count );
    WebSocketDeleteHandle( h );
}

static void test_send(void)
{
    WEB_SOCKET_HANDLE h;
    WEB_SOCKET_BUFFER buf, bufs[2];
    WEB_SOCKET_BUFFER_TYPE type;
    WEB_SOCKET_ACTION action;
    ULONG n;
    void *actx, *cctx;
    HRESULT hr;
    int i;

    trace( "=== send\n" );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    hr = fake_client_handshake( h, TRUE );
    if (FAILED(hr)) { WebSocketDeleteHandle( h ); return; }

    /* nothing pending */
    get_action( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, bufs, &n, &type, &actx, &cctx, &hr );
    get_action( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, bufs, &n, &type, &actx, &cctx, &hr );
    get_action( h, WEB_SOCKET_ALL_ACTION_QUEUE, 2, bufs, &n, &type, &actx, &cctx, &hr );
    get_action( h, 0, 2, bufs, &n, &type, &actx, &cctx, &hr );
    get_action( h, 7, 2, bufs, &n, &type, &actx, &cctx, &hr );

    trace( "-- utf8 message 'hello', 2 buffers\n" );
    memcpy( app_buf, "hello", 5 );
    buf.Data.pbBuffer = app_buf; buf.Data.ulBufferLength = 5;
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &buf, (void *)0x1001 );
    trace( "  Send: hr %#lx\n", hr );
    trace( "-- second send while first pending\n" );
    hr = WebSocketSend( h, WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, &buf, (void *)0x1002 );
    trace( "  Send #2: hr %#lx\n", hr );
    trace( "-- receive queue while only sends pending\n" );
    get_action( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, bufs, &n, &type, &actx, &cctx, &hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );

    trace( "-- utf8 message with 1 buffer offered\n" );
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &buf, (void *)0x1003 );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 1, NULL, 0, 10 );

    trace( "-- GetAction with 0 buffers offered\n" );
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &buf, (void *)0x1004 );
    action = get_action( h, WEB_SOCKET_SEND_ACTION_QUEUE, 0, bufs, &n, &type, &actx, &cctx, &hr );
    if (SUCCEEDED(hr) && action != WEB_SOCKET_NO_ACTION) WebSocketCompleteAction( h, cctx, 0 );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );

    trace( "-- binary fragment + utf8 fragment + binary message (continuation)\n" );
    hr = WebSocketSend( h, WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE, &buf, (void *)0x1005 );
    trace( "  Send fragment: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE, &buf, (void *)0x1006 );
    trace( "  Send utf8 fragment inside binary message: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    hr = WebSocketSend( h, WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, &buf, (void *)0x1007 );
    trace( "  Send final binary: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );

    trace( "-- ping (NULL buffer) and unsolicited pong\n" );
    hr = WebSocketSend( h, WEB_SOCKET_PING_PONG_BUFFER_TYPE, NULL, (void *)0x1008 );
    trace( "  Send ping: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    hr = WebSocketSend( h, WEB_SOCKET_UNSOLICITED_PONG_BUFFER_TYPE, NULL, (void *)0x1009 );
    trace( "  Send unsolicited pong: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    hr = WebSocketSend( h, WEB_SOCKET_PING_PONG_BUFFER_TYPE, &buf, (void *)0x100a );
    trace( "  Send ping with buffer: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );

    trace( "-- empty message, NULL buffer for data type, zero length\n" );
    buf.Data.pbBuffer = app_buf; buf.Data.ulBufferLength = 0;
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &buf, (void *)0x100b );
    trace( "  Send zero length: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, NULL, (void *)0x100c );
    trace( "  Send NULL buffer utf8: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    hr = WebSocketSend( h, 0x12345678, &buf, (void *)0x100d );
    trace( "  Send bad type: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );

    trace( "-- large payload 700 bytes (send buffer %u)\n", SEND_SIZE );
    for (i = 0; i < 700; i++) app_buf[i] = i;
    buf.Data.pbBuffer = app_buf; buf.Data.ulBufferLength = 700;
    hr = WebSocketSend( h, WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, &buf, (void *)0x100e );
    trace( "  Send 700: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 20 );

    trace( "-- short completion (send less than offered)\n" );
    buf.Data.ulBufferLength = 5;
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &buf, (void *)0x100f );
    action = get_action( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, bufs, &n, &type, &actx, &cctx, &hr );
    if (action == WEB_SOCKET_SEND_TO_NETWORK_ACTION) WebSocketCompleteAction( h, cctx, 3 );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    trace( "-- send after short completion\n" );
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &buf, (void *)0x1010 );
    trace( "  Send: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    hr = WebSocketReceive( h, NULL, NULL );
    trace( "  Receive: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, (const BYTE *)"\x81\x02hi", 4, 10 );
    WebSocketDeleteHandle( h );

    trace( "-- close with status and reason, then data / control sends\n" );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    memcpy( app_buf, "bye now", 7 );
    buf.CloseStatus.pbReason = app_buf; buf.CloseStatus.ulReasonLength = 7; buf.CloseStatus.usStatus = 1000;
    hr = WebSocketSend( h, WEB_SOCKET_CLOSE_BUFFER_TYPE, &buf, (void *)0x1011 );
    trace( "  Send close: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    buf.Data.pbBuffer = app_buf; buf.Data.ulBufferLength = 5;
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &buf, (void *)0x1012 );
    trace( "  Send data after close: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    hr = WebSocketSend( h, WEB_SOCKET_PING_PONG_BUFFER_TYPE, NULL, (void *)0x1013 );
    trace( "  Send ping after close: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    hr = WebSocketSend( h, WEB_SOCKET_CLOSE_BUFFER_TYPE, NULL, (void *)0x1014 );
    trace( "  Send second close: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    hr = WebSocketReceive( h, NULL, NULL );
    trace( "  Receive after close sent: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, (const BYTE *)"\x88\x02\x03\xe8", 4, 10 );
    WebSocketDeleteHandle( h );

    trace( "-- close without payload (NULL buffer) and with status 1005 / reason 124\n" );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    buf.CloseStatus.pbReason = app_buf; buf.CloseStatus.ulReasonLength = 124; buf.CloseStatus.usStatus = 1000;
    hr = WebSocketSend( h, WEB_SOCKET_CLOSE_BUFFER_TYPE, &buf, (void *)0x1015 );
    trace( "  Send close reason 124: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    buf.CloseStatus.pbReason = NULL; buf.CloseStatus.ulReasonLength = 0; buf.CloseStatus.usStatus = 1005;
    hr = WebSocketSend( h, WEB_SOCKET_CLOSE_BUFFER_TYPE, &buf, (void *)0x1016 );
    trace( "  Send close status 1005: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    hr = WebSocketSend( h, WEB_SOCKET_CLOSE_BUFFER_TYPE, NULL, (void *)0x1017 );
    trace( "  Send close NULL: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    WebSocketDeleteHandle( h );

    trace( "-- masking disabled\n" );
    h = create_client( TRUE, TRUE, RECV_SIZE, SEND_SIZE, &hr );
    trace( "  create: hr %#lx\n", hr );
    fake_client_handshake( h, FALSE );
    memcpy( app_buf, "hello", 5 );
    buf.Data.pbBuffer = app_buf; buf.Data.ulBufferLength = 5;
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &buf, (void *)0x1018 );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &buf, (void *)0x1019 );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 1, NULL, 0, 10 );
    buf.Data.ulBufferLength = 700;
    hr = WebSocketSend( h, WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, &buf, (void *)0x101a );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    WebSocketDeleteHandle( h );

    trace( "-- no allocated buffer\n" );
    h = create_client( FALSE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    buf.Data.ulBufferLength = 5;
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &buf, (void *)0x101b );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    WebSocketDeleteHandle( h );
}

static void test_receive(void)
{
    static const BYTE mask[4] = { 0x11, 0x22, 0x33, 0x44 };
    WEB_SOCKET_HANDLE h;
    WEB_SOCKET_BUFFER bufs[2], buf;
    WEB_SOCKET_BUFFER_TYPE type;
    WEB_SOCKET_ACTION action;
    BYTE frame[1024], payload[700];
    ULONG n, len;
    void *actx, *cctx;
    HRESULT hr;
    int i;

    trace( "=== receive\n" );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );

    trace( "-- text frame 'world' in one read\n" );
    hr = WebSocketReceive( h, NULL, (void *)0x2001 );
    trace( "  Receive: hr %#lx\n", hr );
    hr = WebSocketReceive( h, NULL, (void *)0x2002 );
    trace( "  Receive #2 while pending: hr %#lx\n", hr );
    get_action( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, bufs, &n, &type, &actx, &cctx, &hr );
    len = build_frame( frame, 1, TRUE, 0, "world", 5, NULL );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 10 );
    trace( "-- queue after the message (second receive, no data)\n" );
    action = get_action( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, bufs, &n, &type, &actx, &cctx, &hr );
    if (action == WEB_SOCKET_RECEIVE_FROM_NETWORK_ACTION)
    {
        trace( "-- feed two frames in one read: 'ab' + 'cd'\n" );
        len = build_frame( frame, 1, TRUE, 0, "ab", 2, NULL );
        len += build_frame( frame + len, 2, TRUE, 0, "cd", 2, NULL );
        memcpy( bufs[0].Data.pbBuffer, frame, len );
        WebSocketCompleteAction( h, cctx, len );
        run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, NULL, 0, 10 );
        trace( "-- next receive should deliver the buffered 'cd' without network\n" );
        hr = WebSocketReceive( h, NULL, (void *)0x2003 );
        run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, NULL, 0, 10 );
    }

    trace( "-- header split: 1 byte, then 1 byte, then payload\n" );
    hr = WebSocketReceive( h, NULL, (void *)0x2004 );
    len = build_frame( frame, 1, TRUE, 0, "split", 5, NULL );
    for (i = 0; i < 3; i++)
    {
        ULONG chunk = i < 2 ? 1 : len - 2;
        action = get_action( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, bufs, &n, &type, &actx, &cctx, &hr );
        if (action != WEB_SOCKET_RECEIVE_FROM_NETWORK_ACTION) break;
        memcpy( bufs[0].Data.pbBuffer, frame + (i < 2 ? i : 2), chunk );
        WebSocketCompleteAction( h, cctx, chunk );
    }
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, NULL, 0, 10 );

    trace( "-- 700 byte binary frame over %u byte receive buffer\n", RECV_SIZE );
    for (i = 0; i < 700; i++) payload[i] = i;
    len = build_frame( frame, 2, TRUE, 0, payload, 700, NULL );
    hr = WebSocketReceive( h, NULL, (void *)0x2005 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    trace( "-- fragmented text: 'fr' (fin=0) + continuation 'ag' (fin=0) + continuation 'ment' (fin=1)\n" );
    len = build_frame( frame, 1, FALSE, 0, "fr", 2, NULL );
    len += build_frame( frame + len, 0, FALSE, 0, "ag", 2, NULL );
    len += build_frame( frame + len, 0, TRUE, 0, "ment", 4, NULL );
    hr = WebSocketReceive( h, NULL, (void *)0x2006 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    hr = WebSocketReceive( h, NULL, (void *)0x2007 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, NULL, 0, 20 );
    hr = WebSocketReceive( h, NULL, (void *)0x2008 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, NULL, 0, 20 );

    trace( "-- ping with payload 'pp', then text 'after'\n" );
    len = build_frame( frame, 9, TRUE, 0, "pp", 2, NULL );
    len += build_frame( frame + len, 1, TRUE, 0, "after", 5, NULL );
    hr = WebSocketReceive( h, NULL, (void *)0x2009 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    trace( "-- send queue after ping (is the pong also there?)\n" );
    get_action( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, bufs, &n, &type, &actx, &cctx, &hr );

    trace( "-- pong with payload 'qq', then text 'after2'\n" );
    len = build_frame( frame, 10, TRUE, 0, "qq", 2, NULL );
    len += build_frame( frame + len, 1, TRUE, 0, "after2", 6, NULL );
    hr = WebSocketReceive( h, NULL, (void *)0x200a );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    hr = WebSocketReceive( h, NULL, (void *)0x200b );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, NULL, 0, 20 );

    trace( "-- receive with application buffer\n" );
    memset( app_buf, 0, 16 );
    buf.Data.pbBuffer = app_buf; buf.Data.ulBufferLength = 16;
    hr = WebSocketReceive( h, &buf, (void *)0x200c );
    trace( "  Receive(app buffer 16): hr %#lx\n", hr );
    len = build_frame( frame, 1, TRUE, 0, "appbuf", 6, NULL );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    dump_bytes( "app_buf after", app_buf, 8 );

    trace( "-- invalid utf8 text frame\n" );
    len = build_frame( frame, 1, TRUE, 0, "\xff\xfe", 2, NULL );
    hr = WebSocketReceive( h, NULL, (void *)0x200d );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    trace( "-- after utf8 error: receive again\n" );
    hr = WebSocketReceive( h, NULL, (void *)0x200e );
    trace( "  Receive: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, (const BYTE *)"\x81\x02hi", 4, 20 );
    WebSocketDeleteHandle( h );

    trace( "-- utf8 verification disabled\n" );
    {
        WEB_SOCKET_PROPERTY props[4];
        ULONG rs = RECV_SIZE, ss = SEND_SIZE;
        BOOL dis = TRUE;
        props[0].Type = WEB_SOCKET_RECEIVE_BUFFER_SIZE_PROPERTY_TYPE; props[0].pvValue = &rs; props[0].ulValueSize = 4;
        props[1].Type = WEB_SOCKET_SEND_BUFFER_SIZE_PROPERTY_TYPE; props[1].pvValue = &ss; props[1].ulValueSize = 4;
        props[2].Type = WEB_SOCKET_ALLOCATED_BUFFER_PROPERTY_TYPE; props[2].pvValue = &alloc_ptr; props[2].ulValueSize = RECV_SIZE + SEND_SIZE + OVERHEAD;
        props[3].Type = WEB_SOCKET_DISABLE_UTF8_VERIFICATION_PROPERTY_TYPE; props[3].pvValue = &dis; props[3].ulValueSize = sizeof(dis);
        hr = WebSocketCreateClientHandle( props, 4, &h );
        trace( "  create: hr %#lx\n", hr );
        fake_client_handshake( h, FALSE );
        hr = WebSocketReceive( h, NULL, (void *)0x200f );
        run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
        WebSocketDeleteHandle( h );
    }

    trace( "-- masked server frame\n" );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    len = build_frame( frame, 1, TRUE, 0, "masked", 6, mask );
    hr = WebSocketReceive( h, NULL, (void *)0x2010 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    hr = WebSocketReceive( h, NULL, (void *)0x2011 );
    trace( "  Receive after error: hr %#lx\n", hr );
    buf.Data.pbBuffer = app_buf; buf.Data.ulBufferLength = 5;
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &buf, NULL );
    trace( "  Send after error: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    WebSocketDeleteHandle( h );

    trace( "-- RSV bit set\n" );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    len = build_frame( frame, 1, TRUE, 4, "rsv", 3, NULL );
    hr = WebSocketReceive( h, NULL, (void *)0x2012 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    WebSocketDeleteHandle( h );

    trace( "-- reserved opcode 3, continuation without start, control frame fin=0, control payload 126\n" );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    len = build_frame( frame, 3, TRUE, 0, "op3", 3, NULL );
    hr = WebSocketReceive( h, NULL, (void *)0x2013 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    WebSocketDeleteHandle( h );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    len = build_frame( frame, 0, TRUE, 0, "cont", 4, NULL );
    hr = WebSocketReceive( h, NULL, (void *)0x2014 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    WebSocketDeleteHandle( h );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    len = build_frame( frame, 9, FALSE, 0, "pf", 2, NULL );
    hr = WebSocketReceive( h, NULL, (void *)0x2015 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    WebSocketDeleteHandle( h );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    len = build_frame( frame, 9, TRUE, 0, payload, 126, NULL );
    hr = WebSocketReceive( h, NULL, (void *)0x2016 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    WebSocketDeleteHandle( h );
    trace( "-- new text frame while a fragmented message is open\n" );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    len = build_frame( frame, 1, FALSE, 0, "fr", 2, NULL );
    len += build_frame( frame + len, 1, TRUE, 0, "new", 3, NULL );
    hr = WebSocketReceive( h, NULL, (void *)0x2017 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    hr = WebSocketReceive( h, NULL, (void *)0x2018 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, NULL, 0, 20 );
    WebSocketDeleteHandle( h );

    trace( "-- 64-bit length header\n" );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    frame[0] = 0x82; frame[1] = 127; memset( frame + 2, 0, 8 ); frame[8] = 0x02; frame[9] = 0xbc; /* 700 */
    memcpy( frame + 10, payload, 700 );
    hr = WebSocketReceive( h, NULL, (void *)0x2019 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, 710, 20 );
    trace( "-- 64-bit length > 4 GiB\n" );
    frame[0] = 0x82; frame[1] = 127; memset( frame + 2, 0, 8 ); frame[5] = 0x01;
    hr = WebSocketReceive( h, NULL, (void *)0x201a );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, 10, 20 );
    WebSocketDeleteHandle( h );

    trace( "-- close frame 1000 'done'\n" );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    payload[0] = 0x03; payload[1] = 0xe8; memcpy( payload + 2, "done", 4 );
    len = build_frame( frame, 8, TRUE, 0, payload, 6, NULL );
    hr = WebSocketReceive( h, NULL, (void *)0x201b );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    trace( "-- after close received: receive, send data, send ping, send close\n" );
    hr = WebSocketReceive( h, NULL, (void *)0x201c );
    trace( "  Receive: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, (const BYTE *)"\x81\x02hi", 4, 20 );
    buf.Data.pbBuffer = app_buf; buf.Data.ulBufferLength = 5;
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &buf, (void *)0x201d );
    trace( "  Send data: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    hr = WebSocketSend( h, WEB_SOCKET_PING_PONG_BUFFER_TYPE, NULL, (void *)0x201e );
    trace( "  Send ping: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    buf.CloseStatus.pbReason = NULL; buf.CloseStatus.ulReasonLength = 0; buf.CloseStatus.usStatus = 1000;
    hr = WebSocketSend( h, WEB_SOCKET_CLOSE_BUFFER_TYPE, &buf, (void *)0x201f );
    trace( "  Send close: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    hr = WebSocketSend( h, WEB_SOCKET_PING_PONG_BUFFER_TYPE, NULL, (void *)0x2020 );
    trace( "  Send ping after both closes: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    WebSocketDeleteHandle( h );

    trace( "-- close frame without payload, with 1-byte payload, with status only, with 123 reason, with invalid utf8 reason\n" );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    len = build_frame( frame, 8, TRUE, 0, NULL, 0, NULL );
    hr = WebSocketReceive( h, NULL, (void *)0x2021 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    WebSocketDeleteHandle( h );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    len = build_frame( frame, 8, TRUE, 0, "\x03", 1, NULL );
    hr = WebSocketReceive( h, NULL, (void *)0x2022 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    WebSocketDeleteHandle( h );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    len = build_frame( frame, 8, TRUE, 0, "\x03\xe9", 2, NULL );
    hr = WebSocketReceive( h, NULL, (void *)0x2023 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    WebSocketDeleteHandle( h );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    payload[0] = 0x03; payload[1] = 0xe8; memset( payload + 2, 'r', 123 );
    len = build_frame( frame, 8, TRUE, 0, payload, 125, NULL );
    hr = WebSocketReceive( h, NULL, (void *)0x2024 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    WebSocketDeleteHandle( h );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    payload[0] = 0x03; payload[1] = 0xe8; payload[2] = 0xff; payload[3] = 0xfe;
    len = build_frame( frame, 8, TRUE, 0, payload, 4, NULL );
    hr = WebSocketReceive( h, NULL, (void *)0x2025 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    WebSocketDeleteHandle( h );
    trace( "-- close status 999 and 1005 and 1006 on the wire\n" );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    len = build_frame( frame, 8, TRUE, 0, "\x03\xe7", 2, NULL );
    hr = WebSocketReceive( h, NULL, (void *)0x2026 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    WebSocketDeleteHandle( h );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    len = build_frame( frame, 8, TRUE, 0, "\x03\xed", 2, NULL );
    hr = WebSocketReceive( h, NULL, (void *)0x2027 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 20 );
    WebSocketDeleteHandle( h );

    trace( "-- receive completed with 0 bytes (connection closed)\n" );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    hr = WebSocketReceive( h, NULL, (void *)0x2028 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, NULL, 0, 20 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, NULL, 0, 20 );
    hr = WebSocketReceive( h, NULL, (void *)0x2029 );
    trace( "  Receive after 0-byte completion: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, (const BYTE *)"\x81\x02hi", 4, 20 );
    WebSocketDeleteHandle( h );

    trace( "-- ping while a send is pending: which queue carries the pong?\n" );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    buf.Data.pbBuffer = app_buf; buf.Data.ulBufferLength = 5;
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &buf, (void *)0x202a );
    hr = WebSocketReceive( h, NULL, (void *)0x202b );
    len = build_frame( frame, 9, TRUE, 0, "pp", 2, NULL );
    action = get_action( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, bufs, &n, &type, &actx, &cctx, &hr );
    if (action == WEB_SOCKET_RECEIVE_FROM_NETWORK_ACTION)
    {
        memcpy( bufs[0].Data.pbBuffer, frame, len );
        WebSocketCompleteAction( h, cctx, len );
    }
    trace( "  send queue now:\n" );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    trace( "  receive queue now:\n" );
    action = get_action( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, bufs, &n, &type, &actx, &cctx, &hr );
    if (SUCCEEDED(hr) && action != WEB_SOCKET_NO_ACTION) WebSocketCompleteAction( h, cctx, action == WEB_SOCKET_SEND_TO_NETWORK_ACTION ? sum_len( bufs, n ) : 0 );
    trace( "  all queue now:\n" );
    action = get_action( h, WEB_SOCKET_ALL_ACTION_QUEUE, 2, bufs, &n, &type, &actx, &cctx, &hr );
    if (SUCCEEDED(hr) && action != WEB_SOCKET_NO_ACTION) WebSocketCompleteAction( h, cctx, 0 );
    WebSocketDeleteHandle( h );
}

static void test_abort(void)
{
    WEB_SOCKET_HANDLE h;
    WEB_SOCKET_BUFFER buf;
    HRESULT hr;

    trace( "=== abort\n" );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    buf.Data.pbBuffer = app_buf; buf.Data.ulBufferLength = 5;
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &buf, (void *)0x3001 );
    hr = WebSocketReceive( h, NULL, (void *)0x3002 );
    WebSocketAbortHandle( h );
    trace( "-- drain send queue (1 buffer, like .NET)\n" );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 1, NULL, 0, 10 );
    trace( "-- drain receive queue\n" );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, NULL, 0, 10 );
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &buf, (void *)0x3003 );
    trace( "  Send after abort: hr %#lx\n", hr );
    hr = WebSocketReceive( h, NULL, (void *)0x3004 );
    trace( "  Receive after abort: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_ALL_ACTION_QUEUE, 2, NULL, 0, 10 );
    WebSocketAbortHandle( h );
    trace( "  abort twice survived\n" );
    WebSocketDeleteHandle( h );

    trace( "-- abort before handshake, then handshake\n" );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    WebSocketAbortHandle( h );
    hr = fake_client_handshake( h, FALSE );
    trace( "  handshake after abort: hr %#lx\n", hr );
    WebSocketDeleteHandle( h );

    trace( "-- abort in the middle of a receive (RECEIVE_FROM_NETWORK outstanding)\n" );
    h = create_client( TRUE, FALSE, RECV_SIZE, SEND_SIZE, &hr );
    fake_client_handshake( h, FALSE );
    hr = WebSocketReceive( h, NULL, (void *)0x3005 );
    {
        WEB_SOCKET_BUFFER bufs[2];
        WEB_SOCKET_BUFFER_TYPE type;
        ULONG n;
        void *actx, *cctx;
        get_action( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, bufs, &n, &type, &actx, &cctx, &hr );
        WebSocketAbortHandle( h );
        WebSocketCompleteAction( h, cctx, 0 );
        run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, NULL, 0, 10 );
    }
    WebSocketDeleteHandle( h );
}

static void test_server(void)
{
    static const BYTE mask[4] = { 0x11, 0x22, 0x33, 0x44 };
    WEB_SOCKET_HTTP_HEADER request[6], *response;
    WEB_SOCKET_PROPERTY props[3];
    WEB_SOCKET_HANDLE h;
    WEB_SOCKET_BUFFER buf;
    ULONG count, rs = RECV_SIZE, ss = 16;
    BYTE frame[64];
    ULONG len;
    HRESULT hr;

    trace( "=== server\n" );
    props[0].Type = WEB_SOCKET_RECEIVE_BUFFER_SIZE_PROPERTY_TYPE; props[0].pvValue = &rs; props[0].ulValueSize = 4;
    props[1].Type = WEB_SOCKET_SEND_BUFFER_SIZE_PROPERTY_TYPE; props[1].pvValue = &ss; props[1].ulValueSize = 4;
    props[2].Type = WEB_SOCKET_ALLOCATED_BUFFER_PROPERTY_TYPE; props[2].pvValue = &alloc_ptr; props[2].ulValueSize = RECV_SIZE + 16 + OVERHEAD;
    memset( alloc_buf, 0xaa, sizeof(alloc_buf) );
    hr = WebSocketCreateServerHandle( props, 3, &h );
    trace( "  create server (send 16): hr %#lx\n", hr );
    if (FAILED(hr)) return;

    /* the .NET fake request */
    request[0] = hdr( "Connection", "Upgrade" );
    request[1] = hdr( "Upgrade", "websocket" );
    request[2] = hdr( "Host", "" );
    request[3] = hdr( "Sec-WebSocket-Version", "13" );
    request[4] = hdr( "Sec-WebSocket-Key", "AAAAAAAAAAAAAAAAAAAAAA==" );
    hr = WebSocketBeginServerHandshake( h, NULL, NULL, 0, request, 5, &response, &count );
    trace( "  BeginServerHandshake: hr %#lx\n", hr );
    if (SUCCEEDED(hr)) dump_headers( "response", response, count );
    hr = WebSocketEndServerHandshake( h );
    trace( "  EndServerHandshake: hr %#lx\n", hr );

    trace( "-- masked client frame 'hello'\n" );
    len = build_frame( frame, 1, TRUE, 0, "hello", 5, mask );
    hr = WebSocketReceive( h, NULL, (void *)0x4001 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 10 );
    trace( "-- unmasked client frame\n" );
    len = build_frame( frame, 1, TRUE, 0, "plain", 5, NULL );
    hr = WebSocketReceive( h, NULL, (void *)0x4002 );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 10 );
    trace( "-- server send 'hello' (2 buffers) and 30 bytes with send buffer 16\n" );
    memcpy( app_buf, "hello world, this is a server!", 30 );
    buf.Data.pbBuffer = app_buf; buf.Data.ulBufferLength = 5;
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &buf, (void *)0x4003 );
    trace( "  Send: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    buf.Data.ulBufferLength = 30;
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &buf, (void *)0x4004 );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    buf.Data.ulBufferLength = 5;
    hr = WebSocketSend( h, WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, &buf, (void *)0x4005 );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 1, NULL, 0, 10 );
    hr = WebSocketSend( h, WEB_SOCKET_PING_PONG_BUFFER_TYPE, NULL, (void *)0x4006 );
    run_queue( h, WEB_SOCKET_SEND_ACTION_QUEUE, 2, NULL, 0, 10 );
    WebSocketDeleteHandle( h );

    trace( "-- server handshake negatives\n" );
    hr = WebSocketCreateServerHandle( props, 3, &h );
    request[3] = hdr( "Sec-WebSocket-Version", "8" );
    hr = WebSocketBeginServerHandshake( h, NULL, NULL, 0, request, 5, &response, &count );
    trace( "  version 8: hr %#lx\n", hr );
    request[3] = hdr( "Sec-WebSocket-Version", "13" );
    hr = WebSocketBeginServerHandshake( h, NULL, NULL, 0, request, 4, &response, &count );
    trace( "  no key: hr %#lx\n", hr );
    request[4] = hdr( "Sec-WebSocket-Key", "short" );
    hr = WebSocketBeginServerHandshake( h, NULL, NULL, 0, request, 5, &response, &count );
    trace( "  short key: hr %#lx\n", hr );
    request[4] = hdr( "Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==" );
    hr = WebSocketBeginServerHandshake( h, NULL, NULL, 0, request + 3, 2, &response, &count );
    trace( "  only version+key: hr %#lx\n", hr );
    if (SUCCEEDED(hr)) dump_headers( "response", response, count );
    WebSocketDeleteHandle( h );

    hr = WebSocketCreateServerHandle( props, 3, &h );
    request[5] = hdr( "Sec-WebSocket-Protocol", "chat, superchat" );
    hr = WebSocketBeginServerHandshake( h, "superchat", NULL, 0, request, 6, &response, &count );
    trace( "  select subprotocol superchat: hr %#lx\n", hr );
    if (SUCCEEDED(hr)) dump_headers( "response", response, count );
    WebSocketDeleteHandle( h );
    hr = WebSocketCreateServerHandle( props, 3, &h );
    hr = WebSocketBeginServerHandshake( h, "nope", NULL, 0, request, 6, &response, &count );
    trace( "  select subprotocol not offered: hr %#lx\n", hr );
    WebSocketDeleteHandle( h );
    hr = WebSocketCreateServerHandle( props, 3, &h );
    {
        const char *ext[1] = { "x-foo" };
        request[5] = hdr( "Sec-WebSocket-Extensions", "x-foo; param=1, x-bar" );
        hr = WebSocketBeginServerHandshake( h, NULL, (PCSTR *)ext, 1, request, 6, &response, &count );
        trace( "  select extension x-foo: hr %#lx\n", hr );
        if (SUCCEEDED(hr)) dump_headers( "response", response, count );
    }
    WebSocketDeleteHandle( h );
    trace( "-- session function right after BeginServerHandshake (implicit end)\n" );
    hr = WebSocketCreateServerHandle( props, 3, &h );
    hr = WebSocketBeginServerHandshake( h, NULL, NULL, 0, request, 5, &response, &count );
    hr = WebSocketReceive( h, NULL, NULL );
    trace( "  Receive without EndServerHandshake: hr %#lx\n", hr );
    run_queue( h, WEB_SOCKET_RECEIVE_ACTION_QUEUE, 1, frame, len, 10 );
    WebSocketDeleteHandle( h );
    trace( "-- client handshake on a server handle\n" );
    hr = WebSocketCreateServerHandle( props, 3, &h );
    hr = WebSocketBeginClientHandshake( h, NULL, 0, NULL, 0, request, 2, &response, &count );
    trace( "  BeginClientHandshake on server handle: hr %#lx\n", hr );
    WebSocketDeleteHandle( h );
}
START_TEST(websocket)
{
    test_global_property();
    test_create_handle();
    test_client_handshake();
    test_send();
    test_receive();
    test_abort();
    test_server();
}
