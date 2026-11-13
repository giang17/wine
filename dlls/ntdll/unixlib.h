/*
 * Ntdll Unix interface
 *
 * Copyright (C) 2020 Alexandre Julliard
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

#ifndef __NTDLL_UNIXLIB_H
#define __NTDLL_UNIXLIB_H

#include "wine/unixlib.h"
#include "wine/list.h"

struct _DISPATCHER_CONTEXT;

struct wine_dbg_write_params
{
    const char  *str;
    unsigned int len;
};

struct wine_server_fd_to_handle_params
{
    int          fd;
    unsigned int access;
    unsigned int attributes;
    HANDLE      *handle;
};

struct wine_server_handle_to_fd_params
{
    HANDLE        handle;
    unsigned int  access;
    int          *unix_fd;
    unsigned int *options;
};

struct wine_spawnvp_params
{
    char       **argv;
    int          wait;
};

struct load_so_dll_params
{
    UNICODE_STRING              nt_name;
    void                      **module;
};

struct unwind_builtin_dll_params
{
    ULONG                       type;
    struct _DISPATCHER_CONTEXT *dispatch;
    CONTEXT                    *context;
};

enum ntdll_unix_funcs
{
    unix_load_so_dll,
    unix_unwind_builtin_dll,
    unix_wine_dbg_write,
    unix_wine_server_call,
    unix_wine_server_fd_to_handle,
    unix_wine_server_handle_to_fd,
    unix_wine_spawnvp,
    unix_system_time_precise,
};

extern unixlib_handle_t __wine_unixlib_handle;

/* Queue entry of a thread blocked in RtlWaitOnAddress. While the thread sleeps
 * the PE side publishes it in TEB->ReservedForPerf, so that the Unix side can
 * deal with it when the thread is terminated from outside: a wake-up spent on
 * a dead waiter is lost, and with critical sections the released lock then
 * sits unowned until the next waiter's timeout. The server signals the thread
 * handle before the thread has run its SIGQUIT handler, so the entry may
 * already have been picked by a waker by then; the wake-up is then handed on
 * to the next waiter on the same address. */
struct wait_on_address_entry
{
    struct list  entry;
    const void  *addr;       /* cleared by the waker that unlinked the entry */
    DWORD        tid;
    const void  *wait_addr;  /* address waited on, kept for handing the wake-up on */
    struct list *queue;      /* queue the entry is linked into */
    LONG        *lock;       /* spinlock of that queue */
};

/* the same entry as written by 32-bit ntdll into the 32-bit TEB of a WoW64 thread */
struct wait_on_address_entry32
{
    ULONG next, prev;        /* struct list */
    ULONG addr;
    DWORD tid;
    ULONG wait_addr;
    ULONG queue;
    ULONG lock;
};

#endif /* __NTDLL_UNIXLIB_H */
