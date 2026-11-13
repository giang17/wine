/*
 * Unix library interface for avrt
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

#ifndef __WINE_AVRT_UNIXLIB_H
#define __WINE_AVRT_UNIXLIB_H

#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/unixlib.h"

/* Both structures contain only fixed-size integers, so the 32-bit PE side
 * shares them with the 64-bit Unix side without a wow64 thunk. */

struct avrt_set_realtime_params
{
    int level;      /* AVRT_PRIORITY of the caller: VERYLOW -2 .. CRITICAL 2 */
    int result;     /* 0 on success, errno otherwise */
    int rtprio;     /* SCHED_FIFO priority that was applied, 0 = elevation disabled */
};

struct avrt_revert_params
{
    int result;     /* 0 on success, errno otherwise */
};

enum avrt_funcs
{
    unix_set_realtime,
    unix_revert,
    unix_funcs_count
};

#endif /* __WINE_AVRT_UNIXLIB_H */
