/*
 * Unix library for avrt: real-time scheduling for MMCSS threads
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "unixlib.h"

/* On Windows an MMCSS task puts the thread into the real-time priority band
 * (22-26 for "Pro Audio" at AVRT_PRIORITY_CRITICAL).  The Linux equivalent is
 * SCHED_FIFO; sched_setscheduler() with pid 0 acts on the calling thread only,
 * which is the thread that called AvSetMmThreadCharacteristics().
 *
 * The CRITICAL level defaults to 5, the SCHED_FIFO priority a JACK client thread
 * (and therefore WineASIO's bufferSwitch thread) runs at, so audio workers end up
 * level with the thread that waits for them and below the audio server itself.
 * WINE_AVRT_RTPRIO overrides the CRITICAL level; 0 disables the elevation. */

static int critical_rtprio(void)
{
    static int cached = -1;
    const char *env;

    if (cached >= 0) return cached;
    cached = 5;
    if ((env = getenv( "WINE_AVRT_RTPRIO" )))
    {
        int value = atoi( env );
        int max = sched_get_priority_max( SCHED_FIFO );
        if (value < 0) value = 0;
        if (max > 0 && value > max) value = max;
        cached = value;
    }
    return cached;
}

static NTSTATUS set_realtime( void *args )
{
    struct avrt_set_realtime_params *params = args;
    struct sched_param sp;
    int prio = critical_rtprio();

    params->result = 0;
    params->rtprio = 0;
    if (!prio) return STATUS_SUCCESS;

    /* CRITICAL = base, HIGH = base - 1, NORMAL = base - 2, LOW = base - 3, VERYLOW = base - 4 */
    prio += params->level - 2;
    if (prio < sched_get_priority_min( SCHED_FIFO )) prio = sched_get_priority_min( SCHED_FIFO );

    memset( &sp, 0, sizeof(sp) );
    sp.sched_priority = prio;
    if (sched_setscheduler( 0, SCHED_FIFO, &sp ))
    {
        params->result = errno;
        return STATUS_SUCCESS;
    }
    params->rtprio = prio;
    return STATUS_SUCCESS;
}

static NTSTATUS revert( void *args )
{
    struct avrt_revert_params *params = args;
    struct sched_param sp;

    memset( &sp, 0, sizeof(sp) );
    params->result = sched_setscheduler( 0, SCHED_OTHER, &sp ) ? errno : 0;
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    set_realtime,
    revert,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count );

#ifdef _WIN64

/* The parameter blocks contain only 32-bit integers, so the 32-bit PE side
 * can use the same entry points. */
const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    set_realtime,
    revert,
};

C_ASSERT( ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count );

#endif  /* _WIN64 */
