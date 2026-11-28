/*
 * Fallbacks for debugging functions when running on Windows
 *
 * Copyright 2019 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * In addition to the permissions in the GNU Lesser General Public License,
 * the authors give you unlimited permission to link the compiled version
 * of this file with other programs, and to distribute those programs
 * without any restriction coming from the use of this file.  (The GNU
 * Lesser General Public License restrictions do apply in other respects;
 * for example, they cover modification of the file, and distribution when
 * not linked into another program.)
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

#ifdef __WINE_PE_BUILD

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/debug.h"

WINE_DECLARE_DEBUG_CHANNEL(pid);
WINE_DECLARE_DEBUG_CHANNEL(timestamp);

static const char * (__cdecl *p__wine_dbg_strdup)( const char *str );
static int (__cdecl *p__wine_dbg_output)( const char *str );
static unsigned char (__cdecl *p__wine_dbg_get_channel_flags)( struct __wine_debug_channel *channel );
static int (__cdecl *p__wine_dbg_header)( enum __wine_debug_class cls,
                                          struct __wine_debug_channel *channel,
                                          const char *function );

static const char * const debug_classes[] = { "fixme", "err", "warn", "trace" };

static unsigned char default_flags = (1 << __WINE_DBCL_ERR) | (1 << __WINE_DBCL_FIXME);
static int nb_debug_options = -1;
static int options_size;
static struct __wine_debug_channel *debug_options;
static DWORD partial_line_tid;  /* id of the last thread to output a partial line */

/* Look up an ntdll export without taking the loader lock: the first use of a debug
 * function in a module can come from a thread that must not wait for that lock, e.g.
 * from a crash handler that suspended a thread inside the loader before it calls
 * MiniDumpWriteDump(). ntdll is one of the first two entries of the load order list;
 * these are linked before the process has a second thread and are never unlinked, so
 * their links can be followed without the lock. */
static void *find_ntdll_export( const char *name )
{
#ifndef __arm64ec__
    static const WCHAR ntdllW[] = L"ntdll.dll";
    const LIST_ENTRY *head = &NtCurrentTeb()->Peb->LdrData->InLoadOrderModuleList;
    const LIST_ENTRY *entry = head->Flink;
    const IMAGE_EXPORT_DIRECTORY *exports;
    const IMAGE_DATA_DIRECTORY *dir;
    const IMAGE_NT_HEADERS *nt;
    const DWORD *names, *functions;
    const WORD *ordinals;
    const char *base = NULL;
    unsigned int i, j;
    int min, max, pos, res;
    DWORD rva;

    for (i = 0; i < 2 && entry != head; i++, entry = entry->Flink)
    {
        const LDR_DATA_TABLE_ENTRY *mod = CONTAINING_RECORD( entry, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks );

        if (mod->BaseDllName.Length != sizeof(ntdllW) - sizeof(WCHAR)) continue;
        for (j = 0; j < ARRAY_SIZE(ntdllW) - 1; j++)
        {
            WCHAR ch = mod->BaseDllName.Buffer[j];
            if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
            if (ch != ntdllW[j]) break;
        }
        if (j < ARRAY_SIZE(ntdllW) - 1) continue;
        base = mod->DllBase;
        break;
    }
    if (!base) return NULL;

    nt = (const IMAGE_NT_HEADERS *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir->VirtualAddress || dir->Size < sizeof(*exports)) return NULL;
    exports = (const IMAGE_EXPORT_DIRECTORY *)(base + dir->VirtualAddress);
    names = (const DWORD *)(base + exports->AddressOfNames);
    ordinals = (const WORD *)(base + exports->AddressOfNameOrdinals);
    functions = (const DWORD *)(base + exports->AddressOfFunctions);

    min = 0;
    max = exports->NumberOfNames - 1;
    while (min <= max)
    {
        pos = (min + max) / 2;
        if (!(res = strcmp( name, base + names[pos] )))
        {
            if (ordinals[pos] >= exports->NumberOfFunctions) return NULL;
            rva = functions[ordinals[pos]];
            /* a forward is left to GetProcAddress() */
            if (!rva || (rva >= dir->VirtualAddress && rva < dir->VirtualAddress + dir->Size)) return NULL;
            return (void *)(base + rva);
        }
        if (res < 0) max = pos - 1;
        else min = pos + 1;
    }
#endif
    return NULL;
}

static void load_func( void **func, const char *name, void *def )
{
    if (!*func)
    {
        void *proc = find_ntdll_export( name );

        if (!proc)
        {
            DWORD err = GetLastError();
            HMODULE module = GetModuleHandleW( L"ntdll.dll" );
            proc = GetProcAddress( module, name );
            SetLastError( err );
        }
        InterlockedExchangePointer( func, proc ? proc : def );
    }
}
#define LOAD_FUNC(name) load_func( (void **)&p ## name, #name, fallback ## name )


/* add a new debug option at the end of the option list */
static void add_option( const char *name, unsigned char set, unsigned char clear )
{
    int min = 0, max = nb_debug_options - 1, pos, res;

    if (!name[0])  /* "all" option */
    {
        default_flags = (default_flags & ~clear) | set;
        return;
    }
    if (strlen(name) >= sizeof(debug_options[0].name)) return;

    while (min <= max)
    {
        pos = (min + max) / 2;
        res = strcmp( name, debug_options[pos].name );
        if (!res)
        {
            debug_options[pos].flags = (debug_options[pos].flags & ~clear) | set;
            return;
        }
        if (res < 0) max = pos - 1;
        else min = pos + 1;
    }
    if (nb_debug_options >= options_size)
    {
        options_size = max( options_size * 2, 16 );
        if (!debug_options)
            debug_options = HeapAlloc( GetProcessHeap(), 0, options_size * sizeof(debug_options[0]) );
        else
            debug_options = HeapReAlloc( GetProcessHeap(), 0, debug_options,
                                         options_size * sizeof(debug_options[0]) );
    }

    pos = min;
    if (pos < nb_debug_options) memmove( &debug_options[pos + 1], &debug_options[pos],
                                         (nb_debug_options - pos) * sizeof(debug_options[0]) );
    strcpy( debug_options[pos].name, name );
    debug_options[pos].flags = (default_flags & ~clear) | set;
    nb_debug_options++;
}

/* parse a set of debugging option specifications and add them to the option list */
static void parse_options( const char *str )
{
    char *opt, *next, *options;
    unsigned int i;

    if (!(options = _strdup(str))) return;
    for (opt = options; opt; opt = next)
    {
        const char *p;
        unsigned char set = 0, clear = 0;

        if ((next = strchr( opt, ',' ))) *next++ = 0;

        p = opt + strcspn( opt, "+-" );
        if (!p[0]) p = opt;  /* assume it's a debug channel name */

        if (p > opt)
        {
            for (i = 0; i < ARRAY_SIZE(debug_classes); i++)
            {
                int len = strlen(debug_classes[i]);
                if (len != (p - opt)) continue;
                if (!memcmp( opt, debug_classes[i], len ))  /* found it */
                {
                    if (*p == '+') set |= 1 << i;
                    else clear |= 1 << i;
                    break;
                }
            }
            if (i == ARRAY_SIZE(debug_classes)) /* bad class name, skip it */
                continue;
        }
        else
        {
            if (*p == '-') clear = ~0;
            else set = ~0;
        }
        if (*p == '+' || *p == '-') p++;
        if (!p[0]) continue;

        if (!strcmp( p, "all" ))
            default_flags = (default_flags & ~clear) | set;
        else
            add_option( p, set, clear );
    }
    free( options );
}

/* initialize all options at startup */
static void init_options(void)
{
    char *wine_debug = getenv("WINEDEBUG");

    nb_debug_options = 0;
    if (wine_debug) parse_options( wine_debug );
}

/* FIXME: this is not 100% thread-safe */
static const char * __cdecl fallback__wine_dbg_strdup( const char *str )
{
    static char *list[32];
    static LONG pos;
    char *ret = strdup( str );
    int idx;

    idx = InterlockedIncrement( &pos ) % ARRAY_SIZE(list);
    free( InterlockedExchangePointer( (void **)&list[idx], ret ));
    return ret;
}

static int __cdecl fallback__wine_dbg_output( const char *str )
{
    size_t len = strlen( str );

    if (!len) return 0;
    InterlockedExchange( (LONG *)&partial_line_tid, str[len - 1] != '\n' ? GetCurrentThreadId() : 0 );
    return fwrite( str, 1, len, stderr );
}

static int __cdecl fallback__wine_dbg_header( enum __wine_debug_class cls,
                                              struct __wine_debug_channel *channel,
                                              const char *function )
{
    char buffer[200], *pos = buffer;

    if (!(__wine_dbg_get_channel_flags( channel ) & (1 << cls))) return -1;

    /* skip header if partial line and no other thread came in between */
    if (partial_line_tid == GetCurrentThreadId()) return 0;

    if (TRACE_ON(timestamp))
    {
        UINT ticks = GetTickCount();
        pos += sprintf( pos, "%3u.%03u:", ticks / 1000, ticks % 1000 );
    }
    if (TRACE_ON(pid)) pos += sprintf( pos, "%04x:", (UINT)GetCurrentProcessId() );
    pos += sprintf( pos, "%04x:", (UINT)GetCurrentThreadId() );
    if (function && cls < ARRAY_SIZE( debug_classes ))
        snprintf( pos, sizeof(buffer) - (pos - buffer), "%s:%s:%s ",
                  debug_classes[cls], channel->name, function );

    return fwrite( buffer, 1, strlen(buffer), stderr );
}

static unsigned char __cdecl fallback__wine_dbg_get_channel_flags( struct __wine_debug_channel *channel )
{
    int min, max, pos, res;

    if (nb_debug_options == -1) init_options();

    min = 0;
    max = nb_debug_options - 1;
    while (min <= max)
    {
        pos = (min + max) / 2;
        res = strcmp( channel->name, debug_options[pos].name );
        if (!res) return debug_options[pos].flags;
        if (res < 0) max = pos - 1;
        else min = pos + 1;
    }
    /* no option for this channel */
    if (channel->flags & (1 << __WINE_DBCL_INIT)) channel->flags = default_flags;
    return default_flags;
}

const char * __cdecl __wine_dbg_strdup( const char *str )
{
    LOAD_FUNC( __wine_dbg_strdup );
    return p__wine_dbg_strdup( str );
}

int __cdecl __wine_dbg_output( const char *str )
{
    LOAD_FUNC( __wine_dbg_output );
    return p__wine_dbg_output( str );
}

unsigned char __cdecl __wine_dbg_get_channel_flags( struct __wine_debug_channel *channel )
{
    LOAD_FUNC( __wine_dbg_get_channel_flags );
    return p__wine_dbg_get_channel_flags( channel );
}

int __cdecl __wine_dbg_header( enum __wine_debug_class cls, struct __wine_debug_channel *channel,
                               const char *function )
{
    LOAD_FUNC( __wine_dbg_header );
    return p__wine_dbg_header( cls, channel, function );
}

#endif  /* __WINE_PE_BUILD */
