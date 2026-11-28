/* Avrt dll implementation
 *
 * Copyright (C) 2009 Maarten Lankhorst
 * Copyright (C) 2026 Giang Nguyen
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "wine/debug.h"
#include "avrt.h"
#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(avrt);

#define AVRT_CALL( func, params ) WINE_UNIX_CALL( unix_ ## func, params )

#define AVRT_TASK_MAGIC 0x74727661  /* 'avrt' */

struct avrt_task
{
    DWORD         magic;
    DWORD         index;
    AVRT_PRIORITY priority;
    BOOL          realtime;   /* the calling thread was raised to SCHED_FIFO */
};

static BOOL unix_available;

/* MMCSS task names that get real-time scheduling.  Windows applies its
 * scheduling categories to every registered task, but also limits them
 * through SystemResponsiveness; Linux has no such net, so only the audio
 * tasks (where a late thread is audible) are raised. */
static const WCHAR * const audio_tasks[] =
{
    L"Pro Audio",
    L"Audio",
    L"Capture",
    L"Playback",
};

static BOOL is_audio_task( const WCHAR *name )
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(audio_tasks); i++)
        if (!wcsicmp( name, audio_tasks[i] )) return TRUE;
    return FALSE;
}

static struct avrt_task *get_task( HANDLE handle )
{
    struct avrt_task *task = handle;

    if (!task || IsBadReadPtr( task, sizeof(*task) ) || task->magic != AVRT_TASK_MAGIC)
    {
        SetLastError( ERROR_INVALID_HANDLE );
        return NULL;
    }
    return task;
}

/* Raise or adjust the calling thread; the task remembers the level that was reached. */
static void apply_priority( struct avrt_task *task, AVRT_PRIORITY priority )
{
    struct avrt_set_realtime_params params;
    static BOOL warned;

    task->priority = priority;
    if (!task->realtime) return;

    params.level = priority;
    if (AVRT_CALL( set_realtime, &params ) || params.result)
    {
        if (!warned++)
            WARN( "cannot raise thread %04lx to SCHED_FIFO (errno %d), MMCSS threads stay at normal priority\n",
                  GetCurrentThreadId(), params.result );
        task->realtime = FALSE;
        return;
    }
    TRACE( "thread %04lx: level %d -> SCHED_FIFO %d\n", GetCurrentThreadId(), priority, params.rtprio );
}

static inline WCHAR *strdupAW(const char *src)
{
    int len;
    WCHAR *dst;

    if (!src) return NULL;
    len = MultiByteToWideChar(CP_ACP, 0, src, -1, NULL, 0);
    if ((dst = malloc(len * sizeof(*dst)))) MultiByteToWideChar(CP_ACP, 0, src, -1, dst, len);
    return dst;
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls( instance );
        unix_available = !__wine_init_unix_call();
        if (!unix_available) WARN( "unix library not available, MMCSS threads stay at normal priority\n" );
        break;
    }
    return TRUE;
}

HANDLE WINAPI AvSetMmThreadCharacteristicsA(const char *name, DWORD *index)
{
    WCHAR *nameW = NULL;
    HANDLE ret;

    if (name && !(nameW = strdupAW(name)))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }

    ret = AvSetMmThreadCharacteristicsW(nameW, index);

    free(nameW);
    return ret;
}

HANDLE WINAPI AvSetMmThreadCharacteristicsW(const WCHAR *name, DWORD *index)
{
    struct avrt_task *task;

    TRACE("(%s,%p)\n", debugstr_w(name), index);

    if (!name)
    {
        SetLastError(ERROR_INVALID_TASK_NAME);
        return NULL;
    }

    if (!index)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return NULL;
    }

    if (!(task = malloc( sizeof(*task) )))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }
    task->magic    = AVRT_TASK_MAGIC;
    task->index    = *index ? *index : 1;
    task->realtime = unix_available && is_audio_task( name );
    *index = task->index;

    /* a freshly registered thread starts at the task's normal level */
    apply_priority( task, AVRT_PRIORITY_NORMAL );
    return task;
}

BOOL WINAPI AvQuerySystemResponsiveness(HANDLE AvrtHandle, ULONG *value)
{
    FIXME("(%p, %p): stub\n", AvrtHandle, value);
    return FALSE;
}

BOOL WINAPI AvRevertMmThreadCharacteristics(HANDLE AvrtHandle)
{
    struct avrt_task *task;
    struct avrt_revert_params params;

    TRACE("(%p)\n", AvrtHandle);

    if (!(task = get_task( AvrtHandle ))) return FALSE;

    if (task->realtime && (AVRT_CALL( revert, &params ) || params.result))
        WARN( "cannot revert thread %04lx to SCHED_OTHER (errno %d)\n", GetCurrentThreadId(), params.result );

    task->magic = 0;
    free( task );
    return TRUE;
}

BOOL WINAPI AvSetMmThreadPriority(HANDLE AvrtHandle, AVRT_PRIORITY prio)
{
    struct avrt_task *task;

    TRACE("(%p)->(%d)\n", AvrtHandle, prio);

    if (!(task = get_task( AvrtHandle ))) return FALSE;
    if (prio < AVRT_PRIORITY_VERYLOW || prio > AVRT_PRIORITY_CRITICAL)
    {
        SetLastError( ERROR_INVALID_PARAMETER );
        return FALSE;
    }

    apply_priority( task, prio );
    return TRUE;
}

HANDLE WINAPI AvSetMmMaxThreadCharacteristicsA(const char *task1, const char *task2, DWORD *index)
{
    WCHAR *task1W = NULL, *task2W = NULL;
    HANDLE ret;

    if (task1 && !(task1W = strdupAW(task1)))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }

    if (task2 && !(task2W = strdupAW(task2)))
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return NULL;
    }

    ret = AvSetMmMaxThreadCharacteristicsW(task1W, task2W, index);

    free(task2W);
    free(task1W);
    return ret;
}

HANDLE WINAPI AvSetMmMaxThreadCharacteristicsW(const WCHAR *task1, const WCHAR *task2, DWORD *index)
{
    TRACE("(%s,%s,%p)\n", debugstr_w(task1), debugstr_w(task2), index);

    if (!task1 || task2)
    {
        SetLastError(ERROR_INVALID_TASK_NAME);
        return NULL;
    }

    return AvSetMmThreadCharacteristicsW(task1, index);
}
