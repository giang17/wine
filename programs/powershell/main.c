/*
 * Copyright 2017 Jactry Zeng for CodeWeavers
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

#include <windows.h>
#include <shellapi.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(powershell);

/* Strip one level of matching quotes; cmd-style argv splitting leaves
 * PowerShell string literals like 'C:\path\file.bat' in place. */
static WCHAR *strip_quotes( WCHAR *str )
{
    size_t len = wcslen( str );
    if (len >= 2 && (str[0] == '\'' || str[0] == '"') && str[len - 1] == str[0])
    {
        str[len - 1] = 0;
        str++;
    }
    return str;
}

/* Minimal Start-Process implementation, sufficient for elevation helpers
 * (e.g. node's sudo-prompt) that invoke
 *   powershell.exe Start-Process -FilePath <file> -WindowStyle hidden -Verb runAs
 * with the parameters as separate arguments.  Returns -1 if the command uses
 * parameters this subset does not understand. */
static int start_process( int argc, WCHAR *argv[], int i )
{
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    WCHAR *file = NULL, *verb = NULL, *params = NULL;
    BOOL wait = FALSE;
    int show = SW_SHOWNORMAL;

    for (i = i + 1; i < argc; i++)
    {
        if (!wcsicmp( argv[i], L"-FilePath" ) && i + 1 < argc)
            file = strip_quotes( argv[++i] );
        else if (!wcsicmp( argv[i], L"-ArgumentList" ) && i + 1 < argc)
            params = strip_quotes( argv[++i] );
        else if (!wcsicmp( argv[i], L"-Verb" ) && i + 1 < argc)
            verb = strip_quotes( argv[++i] );
        else if (!wcsicmp( argv[i], L"-WindowStyle" ) && i + 1 < argc)
        {
            i++;
            if (!wcsicmp( argv[i], L"hidden" )) show = SW_HIDE;
            else if (!wcsicmp( argv[i], L"minimized" )) show = SW_SHOWMINIMIZED;
            else if (!wcsicmp( argv[i], L"maximized" )) show = SW_SHOWMAXIMIZED;
        }
        else if (!wcsicmp( argv[i], L"-Wait" ))
            wait = TRUE;
        else if (argv[i][0] != '-' && !file)
            file = strip_quotes( argv[i] );
        else
            return -1;
    }
    if (!file) return -1;

    /* Processes are not restricted under Wine, so an elevation request
     * reduces to a plain launch. */
    if (verb && wcsicmp( verb, L"runas" )) sei.lpVerb = verb;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpFile = file;
    sei.lpParameters = params;
    sei.nShow = show;

    WINE_TRACE( "starting %s params %s verb %s show %d wait %d\n", wine_dbgstr_w(file),
                wine_dbgstr_w(params), wine_dbgstr_w(verb), show, wait );

    if (!ShellExecuteExW( &sei ))
    {
        WINE_WARN( "failed to start %s, error %lu\n", wine_dbgstr_w(file), GetLastError() );
        return 1;
    }
    if (sei.hProcess)
    {
        if (wait) WaitForSingleObject( sei.hProcess, INFINITE );
        CloseHandle( sei.hProcess );
    }
    return 0;
}

int __cdecl wmain(int argc, WCHAR *argv[])
{
    int i, ret;

    WINE_FIXME("stub.\n");
    for (i = 0; i < argc; i++)
        WINE_FIXME("argv[%d] %s\n", i, wine_dbgstr_w(argv[i]));

    for (i = 1; i < argc; i++)
    {
        /* switches that do not take an argument */
        if (!wcsicmp( argv[i], L"-NoProfile" ) || !wcsicmp( argv[i], L"-NoLogo" )
            || !wcsicmp( argv[i], L"-NonInteractive" ) || !wcsicmp( argv[i], L"-NoExit" )
            || !wcsicmp( argv[i], L"-Sta" ) || !wcsicmp( argv[i], L"-Mta" ))
            continue;
        /* switches whose argument does not start the command */
        if ((!wcsicmp( argv[i], L"-InputFormat" ) || !wcsicmp( argv[i], L"-OutputFormat" )
             || !wcsicmp( argv[i], L"-ExecutionPolicy" ) || !wcsicmp( argv[i], L"-WindowStyle" ))
            && i + 1 < argc)
        {
            i++;
            continue;
        }
        if ((!wcsicmp( argv[i], L"-command" ) || !wcsicmp( argv[i], L"-c" )) && i + 1 < argc)
        {
            if (!wcscmp( argv[i + 1], L"-" ))
            {
                char command[4096], *p;

                ++i;
                while (fgets(command, sizeof(command), stdin))
                {
                    WINE_FIXME("command %s.\n", debugstr_a(command));
                    p = command;
                    while (*p && !isspace(*p)) ++p;
                    *p = 0;
                    if (!stricmp(command, "exit"))
                        break;
                }
                return 0;
            }
            if (!wcsicmp( strip_quotes( argv[i + 1] ), L"Start-Process" )
                && (ret = start_process( argc, argv, i + 1 )) >= 0)
                return ret;
            /* An inline command we did not execute must not report success:
             * installers use these as yes/no probes, and a blanket 0 traps
             * them in impossible states (e.g. "app is running, close it"). */
            return 1;
        }
        if (argv[i][0] != '-')
        {
            if (!wcsicmp( strip_quotes( argv[i] ), L"Start-Process" )
                && (ret = start_process( argc, argv, i )) >= 0)
                return ret;
            return 1;
        }
        /* -File, -EncodedCommand, unknown switches: cannot run what follows */
        return 1;
    }
    return 0;
}
