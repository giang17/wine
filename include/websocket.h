/*
 * Copyright 2023 Vijay Kiran Kamuju
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

#ifndef __WEBSOCKET_H__
#define __WEBSOCKET_H__

#ifdef __cplusplus
extern "C" {
#endif

DECLARE_HANDLE(WEB_SOCKET_HANDLE);

#define WEB_SOCKET_MAX_CLOSE_REASON_LENGTH 123

typedef enum _WEB_SOCKET_CLOSE_STATUS
{
    WEB_SOCKET_SUCCESS_CLOSE_STATUS                = 1000,
    WEB_SOCKET_ENDPOINT_UNAVAILABLE_CLOSE_STATUS   = 1001,
    WEB_SOCKET_PROTOCOL_ERROR_CLOSE_STATUS         = 1002,
    WEB_SOCKET_INVALID_DATA_TYPE_CLOSE_STATUS      = 1003,
    WEB_SOCKET_EMPTY_CLOSE_STATUS                  = 1005,
    WEB_SOCKET_ABORTED_CLOSE_STATUS                = 1006,
    WEB_SOCKET_INVALID_PAYLOAD_CLOSE_STATUS        = 1007,
    WEB_SOCKET_POLICY_VIOLATION_CLOSE_STATUS       = 1008,
    WEB_SOCKET_MESSAGE_TOO_BIG_CLOSE_STATUS        = 1009,
    WEB_SOCKET_UNSUPPORTED_EXTENSIONS_CLOSE_STATUS = 1010,
    WEB_SOCKET_SERVER_ERROR_CLOSE_STATUS           = 1011,
    WEB_SOCKET_SECURE_HANDSHAKE_ERROR_CLOSE_STATUS = 1015,
} WEB_SOCKET_CLOSE_STATUS;

typedef enum _WEB_SOCKET_PROPERTY_TYPE
{
    WEB_SOCKET_RECEIVE_BUFFER_SIZE_PROPERTY_TYPE        = 0,
    WEB_SOCKET_SEND_BUFFER_SIZE_PROPERTY_TYPE           = 1,
    WEB_SOCKET_DISABLE_MASKING_PROPERTY_TYPE            = 2,
    WEB_SOCKET_ALLOCATED_BUFFER_PROPERTY_TYPE           = 3,
    WEB_SOCKET_DISABLE_UTF8_VERIFICATION_PROPERTY_TYPE  = 4,
    WEB_SOCKET_KEEPALIVE_INTERVAL_PROPERTY_TYPE         = 5,
    WEB_SOCKET_SUPPORTED_VERSIONS_PROPERTY_TYPE         = 6,
} WEB_SOCKET_PROPERTY_TYPE;

typedef enum _WEB_SOCKET_BUFFER_TYPE
{
    WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE     = 0x80000000,
    WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE    = 0x80000001,
    WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE   = 0x80000002,
    WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE  = 0x80000003,
    WEB_SOCKET_CLOSE_BUFFER_TYPE            = 0x80000004,
    WEB_SOCKET_PING_PONG_BUFFER_TYPE        = 0x80000005,
    WEB_SOCKET_UNSOLICITED_PONG_BUFFER_TYPE = 0x80000006,
} WEB_SOCKET_BUFFER_TYPE;

typedef enum _WEB_SOCKET_ACTION
{
    WEB_SOCKET_NO_ACTION                        = 0,
    WEB_SOCKET_SEND_TO_NETWORK_ACTION           = 1,
    WEB_SOCKET_INDICATE_SEND_COMPLETE_ACTION    = 2,
    WEB_SOCKET_RECEIVE_FROM_NETWORK_ACTION      = 3,
    WEB_SOCKET_INDICATE_RECEIVE_COMPLETE_ACTION = 4,
} WEB_SOCKET_ACTION, *PWEB_SOCKET_ACTION;

typedef enum _WEB_SOCKET_ACTION_QUEUE
{
    WEB_SOCKET_SEND_ACTION_QUEUE    = 0x1,
    WEB_SOCKET_RECEIVE_ACTION_QUEUE = 0x2,
    WEB_SOCKET_ALL_ACTION_QUEUE     = WEB_SOCKET_SEND_ACTION_QUEUE | WEB_SOCKET_RECEIVE_ACTION_QUEUE,
} WEB_SOCKET_ACTION_QUEUE;

typedef struct _WEB_SOCKET_PROPERTY
{
    WEB_SOCKET_PROPERTY_TYPE Type;
    PVOID                    pvValue;
    ULONG                    ulValueSize;
} WEB_SOCKET_PROPERTY, *PWEB_SOCKET_PROPERTY;

typedef struct _WEB_SOCKET_HTTP_HEADER
{
    PCHAR pcName;
    ULONG ulNameLength;
    PCHAR pcValue;
    ULONG ulValueLength;
} WEB_SOCKET_HTTP_HEADER, *PWEB_SOCKET_HTTP_HEADER;

typedef union _WEB_SOCKET_BUFFER
{
    struct
    {
        PBYTE pbBuffer;
        ULONG ulBufferLength;
    } Data;
    struct
    {
        PBYTE  pbReason;
        ULONG  ulReasonLength;
        USHORT usStatus;
    } CloseStatus;
} WEB_SOCKET_BUFFER, *PWEB_SOCKET_BUFFER;

VOID    WINAPI WebSocketAbortHandle(WEB_SOCKET_HANDLE);
HRESULT WINAPI WebSocketBeginClientHandshake(WEB_SOCKET_HANDLE, PCSTR *, ULONG, PCSTR *, ULONG,
                                             const PWEB_SOCKET_HTTP_HEADER, ULONG,
                                             PWEB_SOCKET_HTTP_HEADER *, ULONG *);
HRESULT WINAPI WebSocketBeginServerHandshake(WEB_SOCKET_HANDLE, PCSTR, PCSTR *, ULONG,
                                             const PWEB_SOCKET_HTTP_HEADER, ULONG,
                                             PWEB_SOCKET_HTTP_HEADER *, ULONG *);
VOID    WINAPI WebSocketCompleteAction(WEB_SOCKET_HANDLE, PVOID, ULONG);
HRESULT WINAPI WebSocketCreateClientHandle(const PWEB_SOCKET_PROPERTY, ULONG, WEB_SOCKET_HANDLE *);
HRESULT WINAPI WebSocketCreateServerHandle(const PWEB_SOCKET_PROPERTY, ULONG, WEB_SOCKET_HANDLE *);
VOID    WINAPI WebSocketDeleteHandle(WEB_SOCKET_HANDLE);
HRESULT WINAPI WebSocketEndClientHandshake(WEB_SOCKET_HANDLE, const PWEB_SOCKET_HTTP_HEADER, ULONG,
                                           ULONG *, ULONG *, ULONG *);
HRESULT WINAPI WebSocketEndServerHandshake(WEB_SOCKET_HANDLE);
HRESULT WINAPI WebSocketGetAction(WEB_SOCKET_HANDLE, WEB_SOCKET_ACTION_QUEUE, WEB_SOCKET_BUFFER *, ULONG *,
                                  WEB_SOCKET_ACTION *, WEB_SOCKET_BUFFER_TYPE *, PVOID *, PVOID *);
HRESULT WINAPI WebSocketGetGlobalProperty(WEB_SOCKET_PROPERTY_TYPE, PVOID, ULONG *);
HRESULT WINAPI WebSocketReceive(WEB_SOCKET_HANDLE, WEB_SOCKET_BUFFER *, PVOID);
HRESULT WINAPI WebSocketSend(WEB_SOCKET_HANDLE, WEB_SOCKET_BUFFER_TYPE, WEB_SOCKET_BUFFER *, PVOID);

#ifdef __cplusplus
}
#endif

#endif /* __WEBSOCKET_H__ */
