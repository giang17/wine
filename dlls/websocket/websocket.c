/*
 * WebSocket Protocol Component
 *
 * Copyright 2023 Vijay Kiran Kamuju
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
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "bcrypt.h"
#include "websocket.h"

#include "wine/debug.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(websocket);

/* WebSocket Protocol Component status codes (facility 0x376). */
#define WSPC_E_INVALID_OPERATION   ((HRESULT)0x83760001)
#define WSPC_E_INVALID_FORMAT      ((HRESULT)0x83760002)
#define WSPC_E_INVALID_EXTENSION   ((HRESULT)0x83760003)
#define WSPC_E_INVALID_SUBPROTOCOL ((HRESULT)0x83760004)
#define WSPC_E_INVALID_VERSION     ((HRESULT)0x83760005)

#ifndef E_ILLEGAL_METHOD_CALL
#define E_ILLEGAL_METHOD_CALL    ((HRESULT)0x8000000EL)
#endif

/* RFC 6455 opcodes. */
#define OP_CONTINUATION 0x0
#define OP_TEXT         0x1
#define OP_BINARY       0x2
#define OP_CLOSE        0x8
#define OP_PING         0x9
#define OP_PONG         0xa

#define DEFAULT_BUFFER_SIZE 4096
#define MIN_RECV_SIZE       256
#define MIN_SEND_SIZE       16
#define BUFFER_OVERHEAD     144
#define HANDSHAKE_GUID      "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* Handle life cycle. */
enum ws_state
{
    STATE_CREATED,            /* after Create, before handshake */
    STATE_CLIENT_HANDSHAKE,   /* after BeginClientHandshake */
    STATE_SERVER_HANDSHAKE,   /* after BeginServerHandshake */
    STATE_OPEN,               /* handshake finished */
    STATE_HANDSHAKE_FAULT,    /* handshake ended in a protocol error */
    STATE_ABORTED,
};

/* Receive side. */
enum rx_state
{
    RX_OPEN,
    RX_CLOSED,   /* a close frame was indicated */
    RX_FAULT,    /* a protocol error was indicated */
};

enum emit_kind
{
    EMIT_NONE,
    EMIT_CHUNK,        /* masked/control: header+masked chunk in send area */
    EMIT_ZC_BOTH,      /* zero-copy: header + payload in one action (2 buffers) */
    EMIT_ZC_HDR,       /* zero-copy: header only (payload action to follow) */
    EMIT_ZC_PAYLOAD,   /* zero-copy: payload only */
};

/* A queued send operation. */
struct send_op
{
    struct list entry;
    void       *context;
    BYTE        first_byte;      /* fin | rsv | opcode */
    BOOL        control;
    const BYTE *payload;         /* application payload (data) or ctrl buffer */
    ULONG       payload_len;
    BYTE        ctrl[2 + WEB_SOCKET_MAX_CLOSE_REASON_LENGTH]; /* close status + reason / ping body */
    /* framing progress */
    BYTE        mask[4];
    BOOL        use_mask;        /* client sets the mask bit (key may be zero) */
    BOOL        zero_copy;       /* payload referenced from the application buffer */
    ULONG       framed;          /* payload bytes emitted so far (masked path) */
    BOOL        hdr_emitted;
    enum emit_kind last_emit;    /* what the previous GetAction produced */
    ULONG       last_chunk;      /* payload bytes in the last masked chunk */
    BOOL        complete_pending;
};

struct websocket
{
    BOOL  server;
    ULONG recv_size, send_size;
    BOOL  disable_mask, disable_utf8;

    BYTE *buf;
    ULONG buf_size;
    BOOL  buf_owned;
    BYTE *recv_area;   /* buf */
    BYTE *send_area;   /* buf + recv_size */

    enum ws_state state;
    BOOL  close_sent;

    /* handshake exchange */
    WEB_SOCKET_HTTP_HEADER hdrs[8];
    ULONG hdr_count;
    char  key[32];
    char  accept[32];
    char  proto_hdr[256];
    char  ext_hdr[512];
    /* offered protocols/extensions remembered between Begin and End (client) */
    char  offered_pool[1024];
    const char *offered_proto[8];
    ULONG offered_proto_count;
    const char *offered_ext[8];
    ULONG offered_ext_count;

    /* send side */
    struct list   send_q;
    struct send_op *send_cur;
    enum  { SA_NONE, SA_NETWORK, SA_COMPLETE } send_action;
    BYTE  send_msg_opcode;    /* opcode of an open outgoing fragmented message, 0 = none */

    /* auto pong queued by a received ping */
    BOOL  pong_pending;
    BYTE  pong_body[125];
    ULONG pong_len;
    BOOL  pong_active;        /* a pong SEND_TO_NETWORK is outstanding */
    int   ping_sent;          /* app pings awaiting a pong indication */

    /* receive side */
    enum rx_state rx_state;
    void *rx_ctxq[8];         /* FIFO of outstanding receive contexts */
    int   rx_qhead, rx_qcount;
    enum  { RA_NONE, RA_NETWORK, RA_INDICATE, RA_PONG } recv_action;
    HRESULT rx_indicate_hr;

    ULONG rx_fill;            /* bytes buffered in recv_area */
    ULONG rx_pos;             /* offset of the current frame in recv_area */
    BOOL  rx_hdr_done;        /* current frame header fully parsed */
    ULONG rx_hdr_len;
    BYTE  rx_opcode;
    BOOL  rx_fin;
    BYTE  rx_rsv;
    BOOL  rx_frame_masked;
    BYTE  rx_frame_mask[4];
    UINT64 rx_payload_len;
    ULONG rx_payload_got;     /* payload bytes already delivered for this frame */

    /* message in progress (across RFC fragments) */
    BYTE  rx_msg_opcode;      /* 0 = none open */
    BOOL  rx_msg_custom;

    /* indicate bookkeeping for the current frame delivery */
    ULONG rx_deliver_off;
    ULONG rx_deliver_len;
    WEB_SOCKET_BUFFER_TYPE rx_deliver_type;
    WEB_SOCKET_CLOSE_STATUS rx_close_status;
    BOOL  rx_close_empty;     /* received close frame had no payload at all */

    /* streaming UTF-8 validation of the current text message */
    int   utf8_need;          /* continuation bytes still expected */
    unsigned int utf8_cp;
    unsigned int utf8_min;

    /* opaque action-context tokens */
    int send_token;
    int recv_token;
};

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

#define RX_ARMED(ws) ((ws)->rx_qcount > 0)
#define RX_CTX(ws)   ((ws)->rx_ctxq[(ws)->rx_qhead])

static void rx_dequeue( struct websocket *ws )
{
    if (ws->rx_qcount) { ws->rx_qhead = (ws->rx_qhead + 1) & 7; ws->rx_qcount--; }
}

static void base64_encode( const BYTE *in, unsigned len, char *out )
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned i = 0, o = 0;
    while (i + 3 <= len)
    {
        out[o++] = b64[in[i] >> 2];
        out[o++] = b64[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        out[o++] = b64[((in[i + 1] & 0xf) << 2) | (in[i + 2] >> 6)];
        out[o++] = b64[in[i + 2] & 0x3f];
        i += 3;
    }
    if (len - i == 1)
    {
        out[o++] = b64[in[i] >> 2];
        out[o++] = b64[(in[i] & 3) << 4];
        out[o++] = '=';
        out[o++] = '=';
    }
    else if (len - i == 2)
    {
        out[o++] = b64[in[i] >> 2];
        out[o++] = b64[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        out[o++] = b64[(in[i + 1] & 0xf) << 2];
        out[o++] = '=';
    }
    out[o] = 0;
}

static void compute_accept( const char *key, char *out )
{
    BYTE digest[20];
    char buf[64];
    int len;

    len = sprintf( buf, "%s%s", key, HANDSHAKE_GUID );
    if (BCryptHash( BCRYPT_SHA1_ALG_HANDLE, NULL, 0, (BYTE *)buf, len, digest, sizeof(digest) ))
        memset( digest, 0, sizeof(digest) );
    base64_encode( digest, sizeof(digest), out );
}

static void generate_key( char *out )
{
    BYTE nonce[16];
    if (BCryptGenRandom( NULL, nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG ))
        memset( nonce, 0, sizeof(nonce) );
    base64_encode( nonce, sizeof(nonce), out );
}

static void gen_mask( struct websocket *ws, BYTE mask[4] )
{
    if (ws->server) { memset( mask, 0, 4 ); return; }
    if (ws->disable_mask) { memset( mask, 0, 4 ); return; }
    if (BCryptGenRandom( NULL, mask, 4, BCRYPT_USE_SYSTEM_PREFERRED_RNG ))
        memset( mask, 0, 4 );
}

static BOOL header_equals( const WEB_SOCKET_HTTP_HEADER *h, const char *name )
{
    size_t n = strlen( name );
    return h->ulNameLength == n && !_strnicmp( h->pcName, name, n );
}

/* find a request/response header by name, returning value+len (value not NUL-terminated) */
static BOOL find_hdr( const WEB_SOCKET_HTTP_HEADER *hdrs, ULONG count, const char *name,
                      const char **value, ULONG *len )
{
    ULONG i;
    for (i = 0; i < count; i++)
    {
        if (header_equals( &hdrs[i], name ))
        {
            *value = hdrs[i].pcValue;
            *len = hdrs[i].ulValueLength;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL value_has_token( const char *value, ULONG len, const char *token )
{
    size_t tlen = strlen( token );
    ULONG i = 0;
    while (i < len)
    {
        ULONG start;
        while (i < len && (value[i] == ' ' || value[i] == ',' || value[i] == '\t')) i++;
        start = i;
        while (i < len && value[i] != ',') i++;
        {
            ULONG end = i;
            while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t')) end--;
            if (end - start == tlen && !_strnicmp( value + start, token, tlen )) return TRUE;
        }
    }
    return FALSE;
}

static void add_response_header( struct websocket *ws, const char *name, const char *value )
{
    WEB_SOCKET_HTTP_HEADER *h = &ws->hdrs[ws->hdr_count++];
    h->pcName = (char *)name;
    h->ulNameLength = strlen( name );
    h->pcValue = (char *)value;
    h->ulValueLength = strlen( value );
}

/* split a comma separated list into trimmed tokens kept in pool */
static ULONG split_list( char *text, const char **out, ULONG max )
{
    ULONG count = 0;
    char *p = text;
    while (*p && count < max)
    {
        char *start, *end;
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;
        start = p;
        while (*p && *p != ',') p++;
        end = p;
        if (*p) *p++ = 0;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        out[count++] = start;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* creation                                                           */
/* ------------------------------------------------------------------ */

static HRESULT create_handle( const WEB_SOCKET_PROPERTY *props, ULONG count, BOOL server,
                              WEB_SOCKET_HANDLE *handle )
{
    struct websocket *ws;
    ULONG i, seen = 0;
    BYTE *user_buf = NULL;
    ULONG user_buf_size = 0;
    ULONG recv_size = DEFAULT_BUFFER_SIZE, send_size = DEFAULT_BUFFER_SIZE;
    BOOL disable_mask = FALSE, disable_utf8 = FALSE;

    if (!handle) return E_INVALIDARG;

    for (i = 0; i < count; i++)
    {
        const WEB_SOCKET_PROPERTY *p = &props[i];
        if (p->Type < 32)
        {
            if (seen & (1u << p->Type)) return E_INVALIDARG; /* duplicate */
            seen |= 1u << p->Type;
        }
        switch (p->Type)
        {
        case WEB_SOCKET_RECEIVE_BUFFER_SIZE_PROPERTY_TYPE:
            if (p->ulValueSize != sizeof(ULONG)) return E_INVALIDARG;
            recv_size = *(ULONG *)p->pvValue;
            if (recv_size < MIN_RECV_SIZE) return E_INVALIDARG;
            break;
        case WEB_SOCKET_SEND_BUFFER_SIZE_PROPERTY_TYPE:
            if (p->ulValueSize != sizeof(ULONG)) return E_INVALIDARG;
            send_size = *(ULONG *)p->pvValue;
            if (send_size < MIN_SEND_SIZE) return E_INVALIDARG;
            break;
        case WEB_SOCKET_DISABLE_MASKING_PROPERTY_TYPE:
            if (p->ulValueSize != sizeof(BOOL)) return E_INVALIDARG;
            disable_mask = *(BOOL *)p->pvValue;
            break;
        case WEB_SOCKET_DISABLE_UTF8_VERIFICATION_PROPERTY_TYPE:
            if (p->ulValueSize != sizeof(BOOL)) return E_INVALIDARG;
            disable_utf8 = *(BOOL *)p->pvValue;
            break;
        case WEB_SOCKET_ALLOCATED_BUFFER_PROPERTY_TYPE:
            user_buf = *(BYTE **)p->pvValue;
            user_buf_size = p->ulValueSize;
            if ((ULONG_PTR)user_buf & 3) return E_INVALIDARG;
            break;
        default:
            return E_INVALIDARG;
        }
    }

    if (!(ws = calloc( 1, sizeof(*ws) ))) return E_OUTOFMEMORY;
    ws->server = server;
    ws->recv_size = recv_size;
    ws->send_size = send_size;
    ws->disable_mask = disable_mask;
    ws->disable_utf8 = disable_utf8;
    list_init( &ws->send_q );

    if (user_buf && user_buf_size >= recv_size + send_size)
    {
        ws->buf = user_buf;
        ws->buf_size = user_buf_size;
        ws->buf_owned = FALSE;
    }
    else
    {
        ws->buf_size = recv_size + send_size + BUFFER_OVERHEAD;
        if (!(ws->buf = malloc( ws->buf_size )))
        {
            free( ws );
            return E_OUTOFMEMORY;
        }
        ws->buf_owned = TRUE;
    }
    ws->recv_area = ws->buf;
    ws->send_area = ws->buf + recv_size;
    ws->state = STATE_CREATED;
    ws->rx_state = RX_OPEN;

    *handle = (WEB_SOCKET_HANDLE)ws;
    return S_OK;
}

HRESULT WINAPI WebSocketCreateClientHandle( const PWEB_SOCKET_PROPERTY properties, ULONG count,
                                            WEB_SOCKET_HANDLE *handle )
{
    TRACE( "(%p, %lu, %p)\n", properties, count, handle );
    return create_handle( properties, count, FALSE, handle );
}

HRESULT WINAPI WebSocketCreateServerHandle( const PWEB_SOCKET_PROPERTY properties, ULONG count,
                                            WEB_SOCKET_HANDLE *handle )
{
    TRACE( "(%p, %lu, %p)\n", properties, count, handle );
    return create_handle( properties, count, TRUE, handle );
}

VOID WINAPI WebSocketAbortHandle( WEB_SOCKET_HANDLE handle )
{
    struct websocket *ws = (struct websocket *)handle;
    TRACE( "(%p)\n", handle );
    if (!ws) return;
    ws->state = STATE_ABORTED;
}

VOID WINAPI WebSocketDeleteHandle( WEB_SOCKET_HANDLE handle )
{
    struct websocket *ws = (struct websocket *)handle;
    struct send_op *op, *next;

    TRACE( "(%p)\n", handle );
    if (!ws) return;
    LIST_FOR_EACH_ENTRY_SAFE( op, next, &ws->send_q, struct send_op, entry )
    {
        list_remove( &op->entry );
        free( op );
    }
    free( ws->send_cur );
    if (ws->buf_owned) free( ws->buf );
    free( ws );
}

/* ------------------------------------------------------------------ */
/* global properties                                                  */
/* ------------------------------------------------------------------ */

HRESULT WINAPI WebSocketGetGlobalProperty( WEB_SOCKET_PROPERTY_TYPE type, PVOID value, ULONG *size )
{
    static const ULONG supported_version = 13;
    static const ULONG keepalive = 30000;

    TRACE( "(%u, %p, %p)\n", type, value, size );

    if (type != WEB_SOCKET_KEEPALIVE_INTERVAL_PROPERTY_TYPE &&
        type != WEB_SOCKET_SUPPORTED_VERSIONS_PROPERTY_TYPE)
        return E_INVALIDARG;
    if (!size) return E_INVALIDARG;
    if (*size < sizeof(ULONG))
    {
        *size = sizeof(ULONG);
        return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    }
    if (!value) return E_INVALIDARG;
    *(ULONG *)value = (type == WEB_SOCKET_KEEPALIVE_INTERVAL_PROPERTY_TYPE) ? keepalive
                                                                           : supported_version;
    *size = sizeof(ULONG);
    return S_OK;
}

/* ------------------------------------------------------------------ */
/* client handshake                                                   */
/* ------------------------------------------------------------------ */

HRESULT WINAPI WebSocketBeginClientHandshake( WEB_SOCKET_HANDLE handle, PCSTR *subprotocols,
        ULONG subprotocol_count, PCSTR *extensions, ULONG extension_count,
        const PWEB_SOCKET_HTTP_HEADER initial_headers, ULONG initial_header_count,
        PWEB_SOCKET_HTTP_HEADER *additional_headers, ULONG *additional_header_count )
{
    struct websocket *ws = (struct websocket *)handle;
    ULONG i, pos;

    TRACE( "(%p, %p, %lu, %p, %lu, %p, %lu, %p, %p)\n", handle, subprotocols, subprotocol_count,
           extensions, extension_count, initial_headers, initial_header_count, additional_headers,
           additional_header_count );

    if (!ws) return E_INVALIDARG;
    if (ws->server) return E_ILLEGAL_METHOD_CALL;
    if (ws->state != STATE_CREATED && ws->state != STATE_CLIENT_HANDSHAKE &&
        ws->state != STATE_ABORTED)
        return E_ILLEGAL_METHOD_CALL;
    if (ws->state == STATE_ABORTED) ws->state = STATE_CREATED;

    generate_key( ws->key );

    /* remember offered protocols/extensions for EndClientHandshake */
    pos = 0;
    ws->offered_proto_count = 0;
    ws->offered_ext_count = 0;
    for (i = 0; i < subprotocol_count && i < 8; i++)
    {
        size_t n = strlen( subprotocols[i] );
        if (pos + n + 1 > sizeof(ws->offered_pool)) break;
        ws->offered_proto[ws->offered_proto_count++] = ws->offered_pool + pos;
        memcpy( ws->offered_pool + pos, subprotocols[i], n + 1 );
        pos += n + 1;
    }
    for (i = 0; i < extension_count && i < 8; i++)
    {
        size_t n = strlen( extensions[i] );
        if (pos + n + 1 > sizeof(ws->offered_pool)) break;
        ws->offered_ext[ws->offered_ext_count++] = ws->offered_pool + pos;
        memcpy( ws->offered_pool + pos, extensions[i], n + 1 );
        pos += n + 1;
    }

    ws->hdr_count = 0;
    add_response_header( ws, "Sec-WebSocket-Key", ws->key );
    add_response_header( ws, "Connection", "Upgrade" );
    add_response_header( ws, "Upgrade", "websocket" );
    add_response_header( ws, "Sec-WebSocket-Version", "13" );
    if (subprotocol_count)
    {
        pos = 0;
        for (i = 0; i < subprotocol_count; i++)
            pos += snprintf( ws->proto_hdr + pos, sizeof(ws->proto_hdr) - pos, "%s%s",
                             i ? ", " : "", subprotocols[i] );
        add_response_header( ws, "Sec-WebSocket-Protocol", ws->proto_hdr );
    }
    if (extension_count)
    {
        pos = 0;
        for (i = 0; i < extension_count; i++)
            pos += snprintf( ws->ext_hdr + pos, sizeof(ws->ext_hdr) - pos, "%s%s",
                             i ? ", " : "", extensions[i] );
        add_response_header( ws, "Sec-WebSocket-Extensions", ws->ext_hdr );
    }

    *additional_headers = ws->hdrs;
    *additional_header_count = ws->hdr_count;
    ws->state = STATE_CLIENT_HANDSHAKE;
    return S_OK;
}

HRESULT WINAPI WebSocketEndClientHandshake( WEB_SOCKET_HANDLE handle,
        const PWEB_SOCKET_HTTP_HEADER response_headers, ULONG response_header_count,
        ULONG *selected_extensions, ULONG *selected_extension_count, ULONG *selected_subprotocol )
{
    struct websocket *ws = (struct websocket *)handle;
    const char *val;
    ULONG vlen, i;
    char accept[32];

    TRACE( "(%p, %p, %lu, %p, %p, %p)\n", handle, response_headers, response_header_count,
           selected_extensions, selected_extension_count, selected_subprotocol );

    if (!ws) return E_INVALIDARG;
    if (ws->state != STATE_CLIENT_HANDSHAKE) return E_ILLEGAL_METHOD_CALL;

    /* any outcome leaves the handshake state */
    ws->state = STATE_HANDSHAKE_FAULT;

    if (!find_hdr( response_headers, response_header_count, "Connection", &val, &vlen ) ||
        !value_has_token( val, vlen, "Upgrade" ))
        return WSPC_E_INVALID_FORMAT;
    if (!find_hdr( response_headers, response_header_count, "Upgrade", &val, &vlen ) ||
        vlen != 9 || _strnicmp( val, "websocket", 9 ))
        return WSPC_E_INVALID_FORMAT;
    if (!find_hdr( response_headers, response_header_count, "Sec-WebSocket-Accept", &val, &vlen ))
        return WSPC_E_INVALID_FORMAT;
    compute_accept( ws->key, accept );
    if (vlen != strlen( accept ) || memcmp( val, accept, vlen ))
        return WSPC_E_INVALID_FORMAT;

    if (find_hdr( response_headers, response_header_count, "Sec-WebSocket-Protocol", &val, &vlen ))
    {
        BOOL found = FALSE;
        for (i = 0; i < ws->offered_proto_count; i++)
        {
            if (strlen( ws->offered_proto[i] ) == vlen && !_strnicmp( ws->offered_proto[i], val, vlen ))
            {
                if (selected_subprotocol) *selected_subprotocol = i;
                found = TRUE;
                break;
            }
        }
        if (!found) return WSPC_E_INVALID_FORMAT;  /* leave *selected_subprotocol untouched */
    }
    else if (selected_subprotocol) *selected_subprotocol = ~0u;

    if (find_hdr( response_headers, response_header_count, "Sec-WebSocket-Extensions", &val, &vlen ))
    {
        char list[512];
        const char *toks[8];
        ULONG n, j, out = 0;
        memcpy( list, val, min( vlen, sizeof(list) - 1 ) );
        list[min( vlen, sizeof(list) - 1 )] = 0;
        n = split_list( list, toks, 8 );
        for (j = 0; j < n; j++)
        {
            BOOL found = FALSE;
            for (i = 0; i < ws->offered_ext_count; i++)
            {
                if (!strcmp( ws->offered_ext[i], toks[j] ))
                {
                    if (selected_extensions && selected_extension_count && out < *selected_extension_count)
                        selected_extensions[out] = i;
                    out++;
                    found = TRUE;
                    break;
                }
            }
            if (!found) return WSPC_E_INVALID_FORMAT;
        }
        if (selected_extension_count)
        {
            if (out > *selected_extension_count)
                return HRESULT_FROM_WIN32( ERROR_MORE_DATA ); /* leave count untouched */
            *selected_extension_count = out;
        }
    }
    else if (selected_extension_count) *selected_extension_count = 0;

    ws->state = STATE_OPEN;
    return S_OK;
}

/* ------------------------------------------------------------------ */
/* server handshake                                                   */
/* ------------------------------------------------------------------ */

HRESULT WINAPI WebSocketBeginServerHandshake( WEB_SOCKET_HANDLE handle, PCSTR subprotocol,
        PCSTR *extensions, ULONG extension_count, const PWEB_SOCKET_HTTP_HEADER request_headers,
        ULONG request_header_count, PWEB_SOCKET_HTTP_HEADER *response_headers,
        ULONG *response_header_count )
{
    struct websocket *ws = (struct websocket *)handle;
    const char *val, *key;
    ULONG vlen, klen, i;

    TRACE( "(%p, %s, %p, %lu, %p, %lu, %p, %p)\n", handle, debugstr_a(subprotocol), extensions,
           extension_count, request_headers, request_header_count, response_headers,
           response_header_count );

    if (!ws) return E_INVALIDARG;
    if (!ws->server) return E_ILLEGAL_METHOD_CALL;
    if (ws->state != STATE_CREATED && ws->state != STATE_SERVER_HANDSHAKE)
        return E_ILLEGAL_METHOD_CALL;

    if (!find_hdr( request_headers, request_header_count, "Connection", &val, &vlen ) ||
        !value_has_token( val, vlen, "Upgrade" ))
        return WSPC_E_INVALID_FORMAT;
    if (!find_hdr( request_headers, request_header_count, "Upgrade", &val, &vlen ) ||
        vlen != 9 || _strnicmp( val, "websocket", 9 ))
        return WSPC_E_INVALID_FORMAT;
    if (!find_hdr( request_headers, request_header_count, "Sec-WebSocket-Version", &val, &vlen ))
        return WSPC_E_INVALID_FORMAT;
    if (vlen != 2 || memcmp( val, "13", 2 ))
        return WSPC_E_INVALID_VERSION;
    if (!find_hdr( request_headers, request_header_count, "Host", &val, &vlen ))
        return WSPC_E_INVALID_FORMAT;
    if (!find_hdr( request_headers, request_header_count, "Sec-WebSocket-Key", &key, &klen ))
        return WSPC_E_INVALID_FORMAT;

    /* verify a requested subprotocol was offered by the client */
    if (subprotocol)
    {
        const char *offered;
        ULONG olen;
        if (!find_hdr( request_headers, request_header_count, "Sec-WebSocket-Protocol", &offered, &olen ) ||
            !value_has_token( offered, olen, subprotocol ))
            return WSPC_E_INVALID_SUBPROTOCOL;
    }

    /* verify each requested extension was offered verbatim by the client */
    if (extension_count)
    {
        const char *offered;
        ULONG olen;
        char list[512];
        const char *toks[16];
        ULONG n, j, k;

        if (!find_hdr( request_headers, request_header_count, "Sec-WebSocket-Extensions", &offered, &olen ))
            return WSPC_E_INVALID_EXTENSION;
        memcpy( list, offered, min( olen, sizeof(list) - 1 ) );
        list[min( olen, sizeof(list) - 1 )] = 0;
        n = split_list( list, toks, 16 );
        for (j = 0; j < extension_count; j++)
        {
            BOOL found = FALSE;
            for (k = 0; k < n; k++)
                if (!strcmp( toks[k], extensions[j] )) { found = TRUE; break; }
            if (!found) return WSPC_E_INVALID_EXTENSION;
        }
    }

    /* build the key echo and the response */
    klen = min( klen, sizeof(ws->key) - 1 );
    memcpy( ws->key, key, klen );
    ws->key[klen] = 0;
    compute_accept( ws->key, ws->accept );

    ws->hdr_count = 0;
    add_response_header( ws, "Sec-WebSocket-Accept", ws->accept );
    add_response_header( ws, "Connection", "Upgrade" );
    add_response_header( ws, "Upgrade", "websocket" );
    if (subprotocol)
    {
        snprintf( ws->proto_hdr, sizeof(ws->proto_hdr), "%s", subprotocol );
        add_response_header( ws, "Sec-WebSocket-Protocol", ws->proto_hdr );
    }
    if (extension_count)
    {
        ULONG pos = 0;
        for (i = 0; i < extension_count; i++)
            pos += snprintf( ws->ext_hdr + pos, sizeof(ws->ext_hdr) - pos, "%s%s",
                             i ? ", " : "", extensions[i] );
        add_response_header( ws, "Sec-WebSocket-Extensions", ws->ext_hdr );
    }

    *response_headers = ws->hdrs;
    *response_header_count = ws->hdr_count;
    ws->state = STATE_SERVER_HANDSHAKE;
    return S_OK;
}

HRESULT WINAPI WebSocketEndServerHandshake( WEB_SOCKET_HANDLE handle )
{
    struct websocket *ws = (struct websocket *)handle;

    TRACE( "(%p)\n", handle );
    if (!ws) return E_INVALIDARG;
    if (ws->state != STATE_SERVER_HANDSHAKE) return E_ILLEGAL_METHOD_CALL;
    ws->state = STATE_OPEN;
    return S_OK;
}

/* ------------------------------------------------------------------ */
/* close status validation                                            */
/* ------------------------------------------------------------------ */

static BOOL close_status_ok_recv( USHORT s )
{
    if (s >= 1000 && s <= 1003) return TRUE;
    if (s >= 1007 && s <= 1011) return TRUE;
    if (s >= 3000) return TRUE;
    return FALSE;
}

static BOOL close_status_ok_send( USHORT s, ULONG reason_len )
{
    if (s == 1005) return reason_len == 0;
    return close_status_ok_recv( s );
}

/* ------------------------------------------------------------------ */
/* send                                                               */
/* ------------------------------------------------------------------ */

static BYTE opcode_for_type( WEB_SOCKET_BUFFER_TYPE type, BOOL *fin, BYTE *rsv, BOOL *control )
{
    *rsv = 0;
    switch (type)
    {
    case WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE:   *fin = TRUE;  *control = FALSE; return OP_TEXT;
    case WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE:  *fin = FALSE; *control = FALSE; return OP_TEXT;
    case WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE: *fin = TRUE;  *control = FALSE; return OP_BINARY;
    case WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE:*fin = FALSE; *control = FALSE; return OP_BINARY;
    case WEB_SOCKET_CLOSE_BUFFER_TYPE:          *fin = TRUE;  *control = TRUE;  return OP_CLOSE;
    case WEB_SOCKET_PING_PONG_BUFFER_TYPE:      *fin = TRUE;  *control = TRUE;  return OP_PING;
    case WEB_SOCKET_UNSOLICITED_PONG_BUFFER_TYPE:*fin = TRUE; *control = TRUE;  return OP_PONG;
    default: return 0xff;
    }
}

HRESULT WINAPI WebSocketSend( WEB_SOCKET_HANDLE handle, WEB_SOCKET_BUFFER_TYPE type,
                              WEB_SOCKET_BUFFER *buffer, PVOID context )
{
    struct websocket *ws = (struct websocket *)handle;
    struct send_op *op;
    BOOL fin, control, data = FALSE;
    BYTE rsv, opcode, base = 0;

    TRACE( "(%p, %#x, %p, %p)\n", handle, type, buffer, context );

    if (!ws) return E_INVALIDARG;
    if (ws->state == STATE_ABORTED) return E_ILLEGAL_METHOD_CALL;
    if (ws->state != STATE_OPEN) return E_ILLEGAL_METHOD_CALL;

    if (type & 0x80000000)
    {
        opcode = opcode_for_type( type, &fin, &rsv, &control );
        if (opcode == 0xff) return E_INVALIDARG;
        if (!control)
        {
            /* a data frame continues the open message as a continuation frame */
            data = TRUE;
            base = opcode;
            if (ws->send_msg_opcode)
            {
                if (ws->send_msg_opcode != base) return WSPC_E_INVALID_FORMAT;
                opcode = OP_CONTINUATION;
            }
        }
    }
    else
    {
        /* custom raw frame: byte0 = ((T & 0xee) >> 1) | (T & 0x100 ? fin : 0) */
        BYTE byte0;
        if (type & ~0x1eeu) return E_INVALIDARG;
        byte0 = ((type & 0xee) >> 1) | ((type & 0x100) ? 0x80 : 0);
        opcode = byte0 & 0x0f;
        fin = !!(byte0 & 0x80);
        rsv = (byte0 >> 4) & 7;
        control = (opcode >= OP_CLOSE);
        /* a standard data/control frame with no reserved bits must use the dedicated type */
        if (!rsv && opcode != OP_CONTINUATION &&
            (opcode == OP_TEXT || opcode == OP_BINARY || opcode >= OP_CLOSE))
            return WSPC_E_INVALID_FORMAT;
    }

    if (ws->close_sent)
    {
        if (opcode == OP_PING || opcode == OP_PONG) /* control still allowed */ ;
        else return WSPC_E_INVALID_FORMAT;
    }

    /* a bare ping/pong carries no application payload */
    if ((opcode == OP_PING || opcode == OP_PONG) && buffer && buffer->Data.ulBufferLength)
        return E_INVALIDARG;

    if (!(op = calloc( 1, sizeof(*op) ))) return E_OUTOFMEMORY;
    op->context = context;
    op->control = control;
    op->use_mask = !ws->server;

    if (opcode == OP_CLOSE)
    {
        USHORT status = buffer ? buffer->CloseStatus.usStatus : 1005;
        ULONG rlen = buffer ? buffer->CloseStatus.ulReasonLength : 0;
        if (rlen > WEB_SOCKET_MAX_CLOSE_REASON_LENGTH) { free( op ); return WSPC_E_INVALID_FORMAT; }
        if (!close_status_ok_send( status, rlen )) { free( op ); return WSPC_E_INVALID_FORMAT; }
        if (status == 1005)
        {
            op->payload_len = 0;
        }
        else
        {
            op->ctrl[0] = status >> 8;
            op->ctrl[1] = status & 0xff;
            if (rlen) memcpy( op->ctrl + 2, buffer->CloseStatus.pbReason, rlen );
            op->payload = op->ctrl;
            op->payload_len = 2 + rlen;
        }
        ws->close_sent = TRUE;
    }
    else if (opcode == OP_PING)
    {
        memcpy( op->ctrl, "ping", 4 );
        op->payload = op->ctrl;
        op->payload_len = 4;
        ws->ping_sent++;
    }
    else if (opcode == OP_PONG)
    {
        op->payload_len = 0;
    }
    else
    {
        op->payload = buffer ? buffer->Data.pbBuffer : NULL;
        op->payload_len = buffer ? buffer->Data.ulBufferLength : 0;
        /* note: native does not reject invalid UTF-8 on send */
        /* data frame on the wire uses zero copy unless the client masks it */
        op->zero_copy = ws->server || ws->disable_mask;
    }

    op->first_byte = (fin ? 0x80 : 0) | (rsv << 4) | opcode;
    gen_mask( ws, op->mask );

    /* track the open fragmented message for standard data frames */
    if (data) ws->send_msg_opcode = fin ? 0 : base;

    list_add_tail( &ws->send_q, &op->entry );
    return S_OK;
}

/* ------------------------------------------------------------------ */
/* receive                                                            */
/* ------------------------------------------------------------------ */

HRESULT WINAPI WebSocketReceive( WEB_SOCKET_HANDLE handle, WEB_SOCKET_BUFFER *buffer, PVOID context )
{
    struct websocket *ws = (struct websocket *)handle;

    TRACE( "(%p, %p, %p)\n", handle, buffer, context );

    if (!ws) return E_INVALIDARG;
    if (ws->state == STATE_ABORTED) return E_ILLEGAL_METHOD_CALL;
    if (ws->state != STATE_OPEN) return E_ILLEGAL_METHOD_CALL;
    if (buffer) return E_INVALIDARG;   /* an application receive buffer is not supported */

    if (ws->rx_qcount < 8)
    {
        ws->rx_ctxq[(ws->rx_qhead + ws->rx_qcount) & 7] = context;
        ws->rx_qcount++;
    }
    return S_OK;
}

/* ------------------------------------------------------------------ */
/* action queue                                                       */
/* ------------------------------------------------------------------ */

static UINT64 frame_payload_len( const BYTE *p, ULONG avail, ULONG *hdr_len, BOOL *need_more,
                                 BOOL *bad )
{
    ULONG base, n;
    UINT64 len;

    *need_more = FALSE;
    *bad = FALSE;
    if (avail < 2) { *need_more = TRUE; return 0; }
    base = (p[1] & 0x7f);
    if (base < 126) { *hdr_len = 2; len = base; }
    else if (base == 126)
    {
        if (avail < 4) { *need_more = TRUE; return 0; }
        len = (p[2] << 8) | p[3];
        if (len < 126) { *bad = TRUE; return 0; }  /* non-minimal */
        *hdr_len = 4;
    }
    else
    {
        if (avail < 10) { *need_more = TRUE; return 0; }
        len = 0;
        for (n = 0; n < 8; n++) len = (len << 8) | p[2 + n];
        if (len <= 0xffff) { *bad = TRUE; return 0; }  /* non-minimal */
        *hdr_len = 10;
    }
    if (p[1] & 0x80) *hdr_len += 4;  /* mask */
    return len;
}

static void utf8_reset( struct websocket *ws )
{
    ws->utf8_need = 0;
    ws->utf8_cp = 0;
    ws->utf8_min = 0;
}

/* returns FALSE on an invalid sequence */
static BOOL utf8_feed( struct websocket *ws, const BYTE *p, ULONG len )
{
    ULONG i;
    for (i = 0; i < len; i++)
    {
        BYTE b = p[i];
        if (!ws->utf8_need)
        {
            if (b < 0x80) continue;
            else if ((b & 0xe0) == 0xc0) { ws->utf8_need = 1; ws->utf8_cp = b & 0x1f; ws->utf8_min = 0x80; }
            else if ((b & 0xf0) == 0xe0) { ws->utf8_need = 2; ws->utf8_cp = b & 0x0f; ws->utf8_min = 0x800; }
            else if ((b & 0xf8) == 0xf0) { ws->utf8_need = 3; ws->utf8_cp = b & 0x07; ws->utf8_min = 0x10000; }
            else return FALSE;
        }
        else
        {
            if ((b & 0xc0) != 0x80) return FALSE;
            ws->utf8_cp = (ws->utf8_cp << 6) | (b & 0x3f);
            if (!--ws->utf8_need)
            {
                if (ws->utf8_cp < ws->utf8_min) return FALSE;       /* overlong */
                if (ws->utf8_cp > 0x10ffff) return FALSE;           /* out of range */
                if (ws->utf8_cp >= 0xd800 && ws->utf8_cp <= 0xdfff) return FALSE; /* surrogate */
            }
        }
    }
    return TRUE;
}

static WEB_SOCKET_BUFFER_TYPE custom_type_from_byte0( BYTE b0 )
{
    return ((b0 & 0x70) << 1) | ((b0 & 0x0f) << 1) | ((b0 & 0x80) ? 0x100 : 0);
}


/* parse the receive buffer and decide the next receive-queue action */
static void rx_advance( struct websocket *ws )
{
    const BYTE *p;
    ULONG hdr_len, avail_after_pos;
    BOOL need_more, bad;

    if (ws->rx_state == RX_FAULT)
    {
        ws->recv_action = RA_INDICATE;
        ws->rx_indicate_hr = E_ILLEGAL_METHOD_CALL;
        return;
    }

    if (!ws->rx_hdr_done)
    {
        UINT64 plen;
        p = ws->recv_area + ws->rx_pos;
        avail_after_pos = ws->rx_fill - ws->rx_pos;
        plen = frame_payload_len( p, avail_after_pos, &hdr_len, &need_more, &bad );
        if (bad) { ws->recv_action = RA_INDICATE; ws->rx_indicate_hr = WSPC_E_INVALID_FORMAT; return; }
        if (need_more)
        {
            if (ws->rx_pos)
            {
                memmove( ws->recv_area, ws->recv_area + ws->rx_pos, avail_after_pos );
                ws->rx_fill = avail_after_pos;
                ws->rx_pos = 0;
            }
            ws->recv_action = RA_NETWORK;
            return;
        }
        ws->rx_hdr_len = hdr_len;
        ws->rx_fin = !!(p[0] & 0x80);
        ws->rx_rsv = (p[0] >> 4) & 7;
        ws->rx_opcode = p[0] & 0x0f;
        ws->rx_frame_masked = !!(p[1] & 0x80);
        if (ws->rx_frame_masked) memcpy( ws->rx_frame_mask, p + hdr_len - 4, 4 );
        ws->rx_payload_len = plen;
        ws->rx_payload_got = 0;

        if (!ws->server && ws->rx_frame_masked) { ws->recv_action = RA_INDICATE; ws->rx_indicate_hr = WSPC_E_INVALID_FORMAT; return; }
        if (ws->server && !ws->rx_frame_masked)  { ws->recv_action = RA_INDICATE; ws->rx_indicate_hr = WSPC_E_INVALID_FORMAT; return; }
        if (ws->rx_opcode >= OP_CLOSE)
        {
            if (!ws->rx_fin || plen > 125) { ws->recv_action = RA_INDICATE; ws->rx_indicate_hr = WSPC_E_INVALID_FORMAT; return; }
        }
        else if (ws->rx_opcode == OP_CONTINUATION)
        {
            if (!ws->rx_msg_opcode && !ws->rx_rsv) { ws->recv_action = RA_INDICATE; ws->rx_indicate_hr = WSPC_E_INVALID_FORMAT; return; }
        }
        else if (!ws->rx_rsv)
        {
            if (ws->rx_msg_opcode) { ws->recv_action = RA_INDICATE; ws->rx_indicate_hr = WSPC_E_INVALID_FORMAT; return; }
        }
        ws->rx_hdr_done = TRUE;
    }

    {
        ULONG payload_off = ws->rx_pos + ws->rx_hdr_len;
        ULONG in_buf = (ws->rx_fill > payload_off) ? ws->rx_fill - payload_off : 0;
        ULONG remaining = (ULONG)ws->rx_payload_len - ws->rx_payload_got;
        BOOL is_control = (ws->rx_opcode >= OP_CLOSE);
        BYTE byte0 = (ws->rx_fin ? 0x80 : 0) | (ws->rx_rsv << 4) | ws->rx_opcode;
        ULONG deliver, i;
        BYTE *pl;

        /* A data frame delivers whatever payload a network read produced, as a
           fragment, as soon as any is available; a control frame needs the whole
           payload before it is processed. */
        if (remaining && (in_buf == 0 || (is_control && in_buf < remaining)))
        {
            /* discard the header so the payload continues at offset 0 */
            if (ws->rx_pos || ws->rx_hdr_len)
            {
                memmove( ws->recv_area, ws->recv_area + payload_off, in_buf );
                ws->rx_fill = in_buf;
                ws->rx_pos = 0;
                ws->rx_hdr_len = 0;
            }
            ws->recv_action = RA_NETWORK;
            return;
        }

        /* a receive after a close frame was indicated is an invalid operation */
        if (ws->rx_state == RX_CLOSED)
        {
            ws->recv_action = RA_INDICATE;
            ws->rx_indicate_hr = WSPC_E_INVALID_OPERATION;
            return;
        }

        payload_off = ws->rx_pos + ws->rx_hdr_len;
        pl = ws->recv_area + payload_off;
        deliver = min( in_buf, remaining );

        if (ws->rx_frame_masked)
            for (i = 0; i < deliver; i++) pl[i] ^= ws->rx_frame_mask[(ws->rx_payload_got + i) & 3];

        if (ws->rx_opcode == OP_CLOSE && !ws->rx_rsv)
        {
            USHORT status = 1005;
            if (ws->rx_payload_len == 1) { ws->recv_action = RA_INDICATE; ws->rx_indicate_hr = WSPC_E_INVALID_FORMAT; return; }
            if (ws->rx_payload_len >= 2)
            {
                status = (pl[0] << 8) | pl[1];
                if (!close_status_ok_recv( status )) { ws->recv_action = RA_INDICATE; ws->rx_indicate_hr = WSPC_E_INVALID_FORMAT; return; }
                if (ws->rx_payload_len > 2 && !ws->disable_utf8)
                {
                    utf8_reset( ws );
                    if (!utf8_feed( ws, pl + 2, (ULONG)ws->rx_payload_len - 2 ) || ws->utf8_need)
                    { ws->recv_action = RA_INDICATE; ws->rx_indicate_hr = WSPC_E_INVALID_FORMAT; return; }
                }
            }
            ws->rx_close_status = status;
            ws->rx_close_empty = (ws->rx_payload_len == 0);
            ws->rx_deliver_off = payload_off + 2;
            ws->rx_deliver_len = (ws->rx_payload_len >= 2) ? (ULONG)ws->rx_payload_len - 2 : 0;
            ws->rx_deliver_type = WEB_SOCKET_CLOSE_BUFFER_TYPE;
            ws->recv_action = RA_INDICATE; ws->rx_indicate_hr = S_OK;
            return;
        }
        if (ws->rx_opcode == OP_PING && !ws->rx_rsv)
        {
            ULONG consumed;
            ws->pong_len = (ULONG)ws->rx_payload_len;
            if (ws->pong_len) memcpy( ws->pong_body, pl, ws->pong_len );
            ws->pong_pending = TRUE;
            consumed = payload_off + (ULONG)ws->rx_payload_len;
            if (consumed >= ws->rx_fill) ws->rx_fill = 0;
            else { memmove( ws->recv_area, ws->recv_area + consumed, ws->rx_fill - consumed ); ws->rx_fill -= consumed; }
            ws->rx_pos = 0;
            ws->rx_hdr_done = FALSE;
            ws->recv_action = RA_PONG;
            return;
        }
        if (ws->rx_opcode == OP_PONG && !ws->rx_rsv)
        {
            ULONG consumed = payload_off + (ULONG)ws->rx_payload_len;
            if (ws->ping_sent > 0)
            {
                /* a solicited pong is indicated to the application */
                ws->rx_deliver_off = 0;
                ws->rx_deliver_len = 0;
                ws->rx_deliver_type = WEB_SOCKET_PING_PONG_BUFFER_TYPE;
                ws->recv_action = RA_INDICATE; ws->rx_indicate_hr = S_OK;
                return;
            }
            /* an unsolicited pong is swallowed; keep receiving */
            if (consumed >= ws->rx_fill) ws->rx_fill = 0;
            else { memmove( ws->recv_area, ws->recv_area + consumed, ws->rx_fill - consumed ); ws->rx_fill -= consumed; }
            ws->rx_pos = 0;
            ws->rx_hdr_done = FALSE;
            rx_advance( ws );
            return;
        }

        if (ws->rx_rsv || (ws->rx_opcode != OP_TEXT && ws->rx_opcode != OP_BINARY &&
                           ws->rx_opcode != OP_CONTINUATION))
        {
            ws->rx_deliver_off = payload_off;
            ws->rx_deliver_len = deliver;
            ws->rx_deliver_type = custom_type_from_byte0( byte0 );
            ws->recv_action = RA_INDICATE; ws->rx_indicate_hr = S_OK;
            return;
        }

        {
            BYTE msg_op = ws->rx_msg_opcode ? ws->rx_msg_opcode : ws->rx_opcode;
            BOOL is_text = (msg_op == OP_TEXT);
            BOOL frame_complete = (ws->rx_payload_got + deliver >= ws->rx_payload_len);
            BOOL msg_complete = frame_complete && ws->rx_fin;

            if (is_text && !ws->disable_utf8)
            {
                if (!ws->rx_msg_opcode && ws->rx_opcode != OP_CONTINUATION && !ws->rx_payload_got)
                    utf8_reset( ws );
                if (!utf8_feed( ws, pl, deliver ))
                { ws->recv_action = RA_INDICATE; ws->rx_indicate_hr = WSPC_E_INVALID_FORMAT; return; }
                if (msg_complete && ws->utf8_need)
                { ws->recv_action = RA_INDICATE; ws->rx_indicate_hr = WSPC_E_INVALID_FORMAT; return; }
            }

            ws->rx_deliver_off = payload_off;
            ws->rx_deliver_len = deliver;
            if (msg_complete)
                ws->rx_deliver_type = is_text ? WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE : WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
            else
                ws->rx_deliver_type = is_text ? WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE : WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE;
            ws->recv_action = RA_INDICATE; ws->rx_indicate_hr = S_OK;
            return;
        }
    }
}

/* consume a delivered receive chunk and advance the parse state */
static void rx_consume_indicate( struct websocket *ws )
{
    ULONG deliver = ws->rx_deliver_len;
    ULONG payload_off = ws->rx_pos + ws->rx_hdr_len;
    BOOL frame_done;

    rx_dequeue( ws );

    if (ws->state == STATE_ABORTED) return;

    if (ws->rx_indicate_hr != S_OK)
    {
        if (ws->rx_indicate_hr == WSPC_E_INVALID_FORMAT) ws->rx_state = RX_FAULT;
        return;
    }

    if (ws->rx_deliver_type == WEB_SOCKET_CLOSE_BUFFER_TYPE)
    {
        ws->rx_state = RX_CLOSED;
        ws->rx_hdr_done = FALSE;
        ws->rx_fill = 0; ws->rx_pos = 0;
        ws->rx_msg_opcode = 0;
        return;
    }
    if (ws->rx_deliver_type == WEB_SOCKET_PING_PONG_BUFFER_TYPE)
    {
        ULONG consumed = payload_off + (ULONG)ws->rx_payload_len;
        if (consumed >= ws->rx_fill) ws->rx_fill = 0;
        else { memmove( ws->recv_area, ws->recv_area + consumed, ws->rx_fill - consumed ); ws->rx_fill -= consumed; }
        ws->rx_pos = 0;
        ws->rx_hdr_done = FALSE;
        if (ws->ping_sent > 0) ws->ping_sent--;
        return;
    }

    ws->rx_payload_got += deliver;
    frame_done = (ws->rx_payload_got >= ws->rx_payload_len);

    if (!ws->rx_rsv && (ws->rx_opcode == OP_TEXT || ws->rx_opcode == OP_BINARY))
        ws->rx_msg_opcode = (frame_done && ws->rx_fin) ? 0 : ws->rx_opcode;
    else if (ws->rx_opcode == OP_CONTINUATION && frame_done && ws->rx_fin)
        ws->rx_msg_opcode = 0;

    if (frame_done)
    {
        ws->rx_pos = payload_off + (ULONG)ws->rx_payload_len;
        ws->rx_hdr_done = FALSE;
        if (ws->rx_pos >= ws->rx_fill) { ws->rx_fill = 0; ws->rx_pos = 0; }
    }
    else
    {
        ws->rx_fill = 0; ws->rx_pos = 0; ws->rx_hdr_len = 0;
    }
}

static ULONG frame_header_len( const struct send_op *op )
{
    ULONG n;
    if (op->payload_len < 126) n = 2;
    else if (op->payload_len <= 0xffff) n = 4;
    else n = 10;
    if (op->use_mask) n += 4;
    return n;
}

static void write_header( BYTE *h, const struct send_op *op )
{
    ULONG o;
    h[0] = op->first_byte;
    if (op->payload_len < 126) { h[1] = op->payload_len; o = 2; }
    else if (op->payload_len <= 0xffff) { h[1] = 126; h[2] = op->payload_len >> 8; h[3] = op->payload_len & 0xff; o = 4; }
    else { ULONG n; h[1] = 127; memset( h + 2, 0, 8 ); for (n = 0; n < 4; n++) h[6 + n] = (op->payload_len >> (24 - 8 * n)) & 0xff; o = 10; }
    if (op->use_mask) { h[1] |= 0x80; memcpy( h + o, op->mask, 4 ); }
}

/* build the next send-to-network action into the send area; returns buffer count */
static ULONG build_send_buffers( struct websocket *ws, struct send_op *op, ULONG max_bufs,
                                 WEB_SOCKET_BUFFER *bufs )
{
    BYTE *area = ws->send_area;
    ULONG hdr_len = frame_header_len( op );

    if (op->zero_copy && op->payload_len)
    {
        if (op->last_emit == EMIT_ZC_HDR)
        {
            bufs[0].Data.pbBuffer = (BYTE *)op->payload;
            bufs[0].Data.ulBufferLength = op->payload_len;
            op->last_emit = EMIT_ZC_PAYLOAD;
            return 1;
        }
        write_header( area, op );
        if (max_bufs >= 2)
        {
            bufs[0].Data.pbBuffer = area;                bufs[0].Data.ulBufferLength = hdr_len;
            bufs[1].Data.pbBuffer = (BYTE *)op->payload; bufs[1].Data.ulBufferLength = op->payload_len;
            op->last_emit = EMIT_ZC_BOTH;
            return 2;
        }
        bufs[0].Data.pbBuffer = area; bufs[0].Data.ulBufferLength = hdr_len;
        op->last_emit = EMIT_ZC_HDR;
        return 1;
    }

    {
        ULONG chunk_hdr = op->hdr_emitted ? 0 : hdr_len;
        ULONG space = ws->send_size - chunk_hdr;
        ULONG chunk = min( space, op->payload_len - op->framed );
        ULONG i;
        if (!op->hdr_emitted) write_header( area, op );
        for (i = 0; i < chunk; i++)
        {
            BYTE b = op->payload[op->framed + i];
            if (op->use_mask) b ^= op->mask[(op->framed + i) & 3];
            area[chunk_hdr + i] = b;
        }
        bufs[0].Data.pbBuffer = area;
        bufs[0].Data.ulBufferLength = chunk_hdr + chunk;
        op->last_emit = EMIT_CHUNK;
        op->last_chunk = chunk;
        return 1;
    }
}

static ULONG build_pong( struct websocket *ws, WEB_SOCKET_BUFFER *bufs )
{
    BYTE *area = ws->send_area;
    BYTE mask[4];
    ULONG hdr = 2, i;

    area[0] = 0x80 | OP_PONG;
    area[1] = ws->pong_len;
    gen_mask( ws, mask );
    if (!ws->server) { area[1] |= 0x80; memcpy( area + 2, mask, 4 ); hdr = 6; }
    for (i = 0; i < ws->pong_len; i++)
    {
        BYTE b = ws->pong_body[i];
        if (!ws->server) b ^= mask[i & 3];
        area[hdr + i] = b;
    }
    bufs[0].Data.pbBuffer = area;
    bufs[0].Data.ulBufferLength = hdr + ws->pong_len;
    return 1;
}

HRESULT WINAPI WebSocketGetAction( WEB_SOCKET_HANDLE handle, WEB_SOCKET_ACTION_QUEUE queue,
        WEB_SOCKET_BUFFER *buffers, ULONG *buffer_count, WEB_SOCKET_ACTION *action,
        WEB_SOCKET_BUFFER_TYPE *type, PVOID *app_context, PVOID *action_context )
{
    struct websocket *ws = (struct websocket *)handle;
    ULONG avail;

    TRACE( "(%p, %#x, %p, %p, %p, %p, %p, %p)\n", handle, queue, buffers, buffer_count, action,
           type, app_context, action_context );

    if (!ws) return E_INVALIDARG;
    if (queue != WEB_SOCKET_SEND_ACTION_QUEUE && queue != WEB_SOCKET_RECEIVE_ACTION_QUEUE &&
        queue != WEB_SOCKET_ALL_ACTION_QUEUE)
        return E_INVALIDARG;
    if (!buffer_count || !action) return E_INVALIDARG;
    if (ws->state != STATE_OPEN && ws->state != STATE_ABORTED) return E_ILLEGAL_METHOD_CALL;
    avail = *buffer_count;

    /* after an abort, pending operations drain to a completion without touching the network */
    if (ws->state == STATE_ABORTED)
    {
        if (queue & WEB_SOCKET_SEND_ACTION_QUEUE)
        {
            if (!ws->send_cur && !list_empty( &ws->send_q ))
            {
                ws->send_cur = LIST_ENTRY( list_head( &ws->send_q ), struct send_op, entry );
                list_remove( &ws->send_cur->entry );
            }
            if (ws->send_cur)
            {
                *action = WEB_SOCKET_INDICATE_SEND_COMPLETE_ACTION;
                *buffer_count = 0;
                if (type) *type = 0;
                *app_context = ws->send_cur->context;
                *action_context = &ws->send_token;
                ws->send_action = SA_COMPLETE;
                return S_OK;
            }
        }
        if ((queue & WEB_SOCKET_RECEIVE_ACTION_QUEUE) && RX_ARMED( ws ))
        {
            *action = WEB_SOCKET_INDICATE_RECEIVE_COMPLETE_ACTION;
            *buffer_count = 0;
            if (type) *type = 0;
            *app_context = RX_CTX( ws );
            *action_context = &ws->recv_token;
            ws->recv_action = RA_INDICATE;
            return S_OK;
        }
        *action = WEB_SOCKET_NO_ACTION;
        *buffer_count = 0;
        if (type) *type = 0;
        *action_context = NULL;
        return S_OK;
    }

    if (queue & WEB_SOCKET_RECEIVE_ACTION_QUEUE)
    {
        if (ws->pong_pending && !ws->pong_active)
        {
            if (!avail) return E_INVALIDARG;
            *buffer_count = build_pong( ws, buffers );
            *action = WEB_SOCKET_SEND_TO_NETWORK_ACTION;
            if (type) *type = WEB_SOCKET_PING_PONG_BUFFER_TYPE;
            *app_context = NULL;
            *action_context = &ws->recv_token;
            ws->pong_active = TRUE;
            ws->recv_action = RA_PONG;
            return S_OK;
        }
        if (RX_ARMED( ws ))
        {
            rx_advance( ws );
            if (ws->recv_action == RA_PONG)
            {
                if (!avail) return E_INVALIDARG;
                *buffer_count = build_pong( ws, buffers );
                *action = WEB_SOCKET_SEND_TO_NETWORK_ACTION;
                if (type) *type = WEB_SOCKET_PING_PONG_BUFFER_TYPE;
                *app_context = NULL;
                *action_context = &ws->recv_token;
                ws->pong_active = TRUE;
                return S_OK;
            }
            if (ws->recv_action == RA_NETWORK)
            {
                if (!avail) return E_INVALIDARG;
                buffers[0].Data.pbBuffer = ws->recv_area + ws->rx_fill;
                buffers[0].Data.ulBufferLength = ws->recv_size - ws->rx_fill;
                *buffer_count = 1;
                *action = WEB_SOCKET_RECEIVE_FROM_NETWORK_ACTION;
                if (type) *type = 0;
                *app_context = RX_CTX( ws );
                *action_context = &ws->recv_token;
                return S_OK;
            }
            if (ws->recv_action == RA_INDICATE)
            {
                *action = WEB_SOCKET_INDICATE_RECEIVE_COMPLETE_ACTION;
                if (ws->rx_indicate_hr != S_OK)
                {
                    HRESULT hr = ws->rx_indicate_hr;
                    *buffer_count = 0;
                    if (type) *type = 0;
                    *app_context = RX_CTX( ws );
                    *action_context = NULL;
                    rx_consume_indicate( ws );
                    return hr;
                }
                if (!avail) return E_INVALIDARG;
                if (ws->rx_deliver_type == WEB_SOCKET_CLOSE_BUFFER_TYPE)
                {
                    buffers[0].CloseStatus.pbReason = ws->rx_close_empty ? NULL : ws->recv_area + ws->rx_deliver_off;
                    buffers[0].CloseStatus.ulReasonLength = ws->rx_deliver_len;
                    buffers[0].CloseStatus.usStatus = ws->rx_close_status;
                }
                else if (ws->rx_deliver_type == WEB_SOCKET_PING_PONG_BUFFER_TYPE)
                {
                    buffers[0].Data.pbBuffer = NULL;
                    buffers[0].Data.ulBufferLength = 0;
                }
                else
                {
                    buffers[0].Data.pbBuffer = ws->recv_area + ws->rx_deliver_off;
                    buffers[0].Data.ulBufferLength = ws->rx_deliver_len;
                }
                *buffer_count = 1;
                if (type) *type = ws->rx_deliver_type;
                *app_context = RX_CTX( ws );
                *action_context = &ws->recv_token;
                return S_OK;
            }
        }
    }

    if (queue & WEB_SOCKET_SEND_ACTION_QUEUE)
    {
        if (!ws->send_cur && !list_empty( &ws->send_q ))
        {
            ws->send_cur = LIST_ENTRY( list_head( &ws->send_q ), struct send_op, entry );
            list_remove( &ws->send_cur->entry );
        }
        if (ws->send_cur)
        {
            if (ws->send_cur->complete_pending)
            {
                *action = WEB_SOCKET_INDICATE_SEND_COMPLETE_ACTION;
                *buffer_count = 0;
                if (type) *type = 0;
                *app_context = ws->send_cur->context;
                *action_context = &ws->send_token;
                ws->send_action = SA_COMPLETE;
                return S_OK;
            }
            if (!avail) return E_INVALIDARG;
            *buffer_count = build_send_buffers( ws, ws->send_cur, avail, buffers );
            *action = WEB_SOCKET_SEND_TO_NETWORK_ACTION;
            if (type) *type = 0;
            *app_context = ws->send_cur->context;
            *action_context = &ws->send_token;
            ws->send_action = SA_NETWORK;
            return S_OK;
        }
    }

    *action = WEB_SOCKET_NO_ACTION;
    *buffer_count = 0;
    if (type) *type = 0;
    *action_context = NULL;
    return S_OK;
}

VOID WINAPI WebSocketCompleteAction( WEB_SOCKET_HANDLE handle, PVOID action_context,
                                     ULONG bytes_transferred )
{
    struct websocket *ws = (struct websocket *)handle;

    TRACE( "(%p, %p, %lu)\n", handle, action_context, bytes_transferred );
    if (!ws) return;

    if (action_context == &ws->send_token && ws->send_cur)
    {
        struct send_op *op = ws->send_cur;
        if (ws->send_action == SA_COMPLETE)
        {
            ws->send_cur = NULL;
            free( op );
            return;
        }
        switch (op->last_emit)
        {
        case EMIT_ZC_BOTH:
        case EMIT_ZC_PAYLOAD:
            op->complete_pending = TRUE;
            break;
        case EMIT_ZC_HDR:
            op->hdr_emitted = TRUE;
            break;
        case EMIT_CHUNK:
            op->hdr_emitted = TRUE;
            op->framed += op->last_chunk;
            if (op->framed >= op->payload_len) op->complete_pending = TRUE;
            break;
        default:
            break;
        }
        return;
    }

    if (action_context == &ws->recv_token)
    {
        if (ws->pong_active)
        {
            ws->pong_pending = FALSE;
            ws->pong_active = FALSE;
            ws->recv_action = RA_NONE;
            return;
        }
        if (ws->recv_action == RA_NETWORK)
        {
            ws->rx_fill += bytes_transferred;
            ws->recv_action = RA_NONE;
            return;
        }
        if (ws->recv_action == RA_INDICATE)
        {
            rx_consume_indicate( ws );
            ws->recv_action = RA_NONE;
            return;
        }
    }
}
