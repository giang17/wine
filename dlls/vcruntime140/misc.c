/*
 * Copyright 2015 Martin Storsjo
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

#include "windows.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(vcruntime);

int* CDECL __processing_throw(void);

/*********************************************************************
 *              __telemetry_main_invoke_trigger
 */
void CDECL __telemetry_main_invoke_trigger(HINSTANCE hinst)
{
    FIXME("(%p)\n", hinst);
}

/*********************************************************************
 *              __telemetry_main_return_trigger
 */
void CDECL __telemetry_main_return_trigger(HINSTANCE hinst)
{
    FIXME("(%p)\n", hinst);
}

/*********************************************************************
 *              __vcrtInitializeCriticalSectionEx
 */
BOOL CDECL __vcrt_InitializeCriticalSectionEx(
        CRITICAL_SECTION *cs, DWORD spin_count, DWORD flags)
{
    TRACE("(%p %lx %lx)\n", cs, spin_count, flags);
    return InitializeCriticalSectionEx(cs, spin_count, flags);
}

int __cdecl __uncaught_exceptions(void)
{
    return *__processing_throw();
}

#ifndef __i386__

#define CXX_EXCEPTION 0xe06d7363

EXCEPTION_DISPOSITION WINAPI __C_specific_handler( EXCEPTION_RECORD *rec, void *frame,
                                                   CONTEXT *context, DISPATCHER_CONTEXT *dispatch );
void CDECL terminate(void);

/*********************************************************************
 *              __C_specific_handler_noexcept
 *
 * SEH handler installed by MSVC for noexcept functions that contain
 * __try scopes. Behaves like __C_specific_handler, except that a C++
 * exception which no filter in this frame accepts terminates the
 * process instead of propagating out of the noexcept frame.
 */
EXCEPTION_DISPOSITION WINAPI __C_specific_handler_noexcept( EXCEPTION_RECORD *rec, void *frame,
                                                            CONTEXT *context, DISPATCHER_CONTEXT *dispatch )
{
    EXCEPTION_DISPOSITION ret = __C_specific_handler( rec, frame, context, dispatch );

    TRACE( "%p %p %p %p -> %u\n", rec, frame, context, dispatch, ret );

    if (ret == ExceptionContinueSearch && rec->ExceptionCode == CXX_EXCEPTION
        && !(rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)))
    {
        ERR( "C++ exception escaping a noexcept frame %p, terminating\n", frame );
        terminate();
    }
    return ret;
}

#endif /* __i386__ */
