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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(powershell);

/* This is not a PowerShell interpreter.  It runs the small subset that
 * installers and launchers use to start other programs -- Start-Process
 * (also on the right-hand side of "$var = " / "$var += "), Wait-Process and
 * New-Item -ItemType Directory -- from a -Command string, a -File script or a
 * bare command line, and reports the outcome honestly: 1 when nothing was
 * executed, otherwise 0 unless a process the script waited for failed, in
 * which case that process's exit code is returned.  Everything else
 * (assignments, conditions, exit statements, sleeps, log polling) is skipped:
 * scripts of this kind wait for a child process and hand its result on, and
 * the child's exit code is that result. */

struct script
{
    HANDLE procs[64];       /* started but not yet awaited */
    unsigned int count;
    DWORD failure;          /* exit code of the first awaited process that failed */
    BOOL started;           /* at least one process was started */
};

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

static BOOL is_space( WCHAR c )
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* Split one statement into words.  Both quote characters delimit a single
 * word (-ArgumentList is usually written with single quotes); an unquoted
 * '|' is a word of its own so pipelines can be split afterwards. */
static int tokenize( WCHAR *cmd, WCHAR **argv, int max )
{
    int argc = 0;
    WCHAR *p = cmd;

    while (*p && argc < max)
    {
        while (is_space( *p )) p++;
        if (!*p) break;
        if (*p == '"' || *p == '\'')
        {
            WCHAR quote = *p;
            argv[argc++] = ++p;
            while (*p && *p != quote) p++;
        }
        else if (*p == '|')
        {
            argv[argc++] = p++;
            if (*p) { *p++ = 0; continue; }
            break;
        }
        else
        {
            argv[argc++] = p;
            while (*p && !is_space( *p ) && *p != '|') p++;
            if (*p == '|') continue;
        }
        if (*p) *p++ = 0;
    }
    return argc;
}

static BOOL is_start_process( const WCHAR *word )
{
    return !wcsicmp( word, L"Start-Process" ) || !wcsicmp( word, L"start" );
}

static void wait_process( struct script *s, HANDLE process )
{
    DWORD code = 0;

    WaitForSingleObject( process, INFINITE );
    GetExitCodeProcess( process, &code );
    WINE_TRACE( "process %p exited with %lu\n", process, code );
    if (code && !s->failure) s->failure = code;
}

static void wait_all( struct script *s )
{
    unsigned int i;

    for (i = 0; i < s->count; i++)
    {
        wait_process( s, s->procs[i] );
        CloseHandle( s->procs[i] );
    }
    s->count = 0;
}

/* Start-Process [-FilePath] <file> [-ArgumentList <args>] [-Verb <verb>]
 * [-WindowStyle <style>] [-WorkingDirectory <dir>] [-Wait] [-PassThru]
 * [-NoNewWindow].  Returns -1 for a form this subset does not understand;
 * nothing is started then. */
static int start_process( struct script *s, int argc, WCHAR *argv[], int i )
{
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    WCHAR *file = NULL, *verb = NULL, *params = NULL, *dir = NULL;
    BOOL wait = FALSE;
    int show = SW_SHOWNORMAL;

    for (i = i + 1; i < argc; i++)
    {
        if (!wcsicmp( argv[i], L"-FilePath" ) && i + 1 < argc)
            file = strip_quotes( argv[++i] );
        else if (!wcsicmp( argv[i], L"-ArgumentList" ) && i + 1 < argc)
            params = strip_quotes( argv[++i] );
        else if (!wcsicmp( argv[i], L"-WorkingDirectory" ) && i + 1 < argc)
            dir = strip_quotes( argv[++i] );
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
        else if (!wcsicmp( argv[i], L"-PassThru" ) || !wcsicmp( argv[i], L"-NoNewWindow" ))
            continue;   /* the handle is kept in any case; there is no console to share */
        else if (argv[i][0] != '-' && !file)
            file = strip_quotes( argv[i] );
        else
        {
            WINE_FIXME( "unsupported Start-Process parameter %s\n", wine_dbgstr_w(argv[i]) );
            return -1;
        }
    }
    if (!file) return -1;

    /* Processes are not restricted under Wine, so an elevation request
     * reduces to a plain launch. */
    if (verb && wcsicmp( verb, L"runas" )) sei.lpVerb = verb;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpFile = file;
    sei.lpParameters = params;
    sei.lpDirectory = dir;
    sei.nShow = show;

    WINE_TRACE( "starting %s params %s verb %s show %d wait %d\n", wine_dbgstr_w(file),
                wine_dbgstr_w(params), wine_dbgstr_w(verb), show, wait );

    if (!ShellExecuteExW( &sei ))
    {
        WINE_WARN( "failed to start %s, error %lu\n", wine_dbgstr_w(file), GetLastError() );
        if (!s->failure) s->failure = 1;
        return 1;
    }
    s->started = TRUE;
    if (sei.hProcess)
    {
        if (wait)
        {
            wait_process( s, sei.hProcess );
            CloseHandle( sei.hProcess );
        }
        else if (s->count < ARRAY_SIZE(s->procs))
            s->procs[s->count++] = sei.hProcess;
        else
            CloseHandle( sei.hProcess );
    }
    return 0;
}

/* New-Item -ItemType Directory [-Path] <path> [-Force]: installers create
 * the directory their /LOG= file goes to, and Inno Setup aborts when it is
 * missing.  Other item types are not created. */
static void new_item( int argc, WCHAR *argv[], int i )
{
    WCHAR *path = NULL, *type = NULL, full[1024];
    int ret;

    for (i = i + 1; i < argc; i++)
    {
        if (!wcsicmp( argv[i], L"-ItemType" ) && i + 1 < argc)
            type = strip_quotes( argv[++i] );
        else if (!wcsicmp( argv[i], L"-Path" ) && i + 1 < argc)
            path = strip_quotes( argv[++i] );
        else if (argv[i][0] != '-' && !path)
            path = strip_quotes( argv[i] );
    }
    if (!path || !type || wcsicmp( type, L"Directory" ))
    {
        WINE_FIXME( "not creating %s of type %s\n", wine_dbgstr_w(path), wine_dbgstr_w(type) );
        return;
    }
    if (!GetFullPathNameW( path, ARRAY_SIZE(full), full, NULL )) return;
    ret = SHCreateDirectoryExW( NULL, full, NULL );
    WINE_TRACE( "creating directory %s: %d\n", wine_dbgstr_w(full), ret );
}

/* One pipeline segment: an optional "$var =" / "$var +=" followed by the
 * command and its parameters, already split into words. */
static void run_command( struct script *s, int argc, WCHAR *argv[] )
{
    WCHAR *cmd;
    int i = 0;

    if (argc >= 3 && argv[0][0] == '$' && (!wcscmp( argv[1], L"=" ) || !wcscmp( argv[1], L"+=" )))
        i = 2;
    if (i >= argc) return;

    cmd = strip_quotes( argv[i] );
    if (is_start_process( cmd ))
        start_process( s, argc, argv, i );
    else if (!wcsicmp( cmd, L"Wait-Process" ))
        wait_all( s );
    else if (!wcsicmp( cmd, L"New-Item" ))
        new_item( argc, argv, i );
    else
        WINE_TRACE( "skipping %s\n", wine_dbgstr_w(cmd) );
}

/* One statement, split at unquoted '|' into pipeline segments. */
static void run_statement( struct script *s, WCHAR *stmt )
{
    WCHAR *tok[64];
    int n, i, start = 0;

    n = tokenize( stmt, tok, ARRAY_SIZE(tok) );
    for (i = 0; i <= n; i++)
    {
        if (i < n && wcscmp( tok[i], L"|" )) continue;
        if (i > start) run_command( s, i - start, tok + start );
        start = i + 1;
    }
}

/* Statements end at an unquoted ';' or newline; '#' starts a comment line. */
static void run_script( struct script *s, WCHAR *text )
{
    WCHAR *p = text, *stmt;

    while (*p)
    {
        WCHAR quote = 0;

        stmt = p;
        while (*p && (quote || (*p != ';' && *p != '\n')))
        {
            if (quote) { if (*p == quote) quote = 0; }
            else if (*p == '\'' || *p == '"') quote = *p;
            p++;
        }
        if (*p) *p++ = 0;
        while (is_space( *stmt )) stmt++;
        if (*stmt && *stmt != '#') run_statement( s, stmt );
    }
}

static int script_result( struct script *s )
{
    unsigned int i;

    /* Processes nobody waited for keep running, as they would in PowerShell. */
    for (i = 0; i < s->count; i++) CloseHandle( s->procs[i] );
    s->count = 0;

    /* A command we did not execute must not report success: installers use
     * these as yes/no probes, and a blanket 0 traps them in impossible states
     * (e.g. "app is running, close it"). */
    if (!s->started) return 1;
    return s->failure;
}

/* Read a -File script: UTF-16LE or UTF-8 with BOM, otherwise UTF-8 and, if
 * that does not decode, the ANSI code page. */
static WCHAR *read_script( const WCHAR *path )
{
    HANDLE file;
    DWORD size, read = 0, len;
    char *data;
    WCHAR *text;

    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE)
    {
        WINE_WARN( "cannot open %s, error %lu\n", wine_dbgstr_w(path), GetLastError() );
        return NULL;
    }
    size = GetFileSize( file, NULL );
    if (size == INVALID_FILE_SIZE || !(data = malloc( size + 2 )))
    {
        CloseHandle( file );
        return NULL;
    }
    ReadFile( file, data, size, &read, NULL );
    CloseHandle( file );

    if (read >= 2 && (BYTE)data[0] == 0xff && (BYTE)data[1] == 0xfe)
    {
        if (!(text = malloc( read )))
        {
            free( data );
            return NULL;
        }
        memcpy( text, data + 2, read - 2 );
        text[(read - 2) / sizeof(WCHAR)] = 0;
    }
    else
    {
        const char *src = data;
        UINT cp = CP_UTF8;

        if (read >= 3 && !memcmp( data, "\xef\xbb\xbf", 3 ))
        {
            src += 3;
            read -= 3;
        }
        len = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, src, read, NULL, 0 );
        if (!len && read)
        {
            cp = CP_ACP;
            len = MultiByteToWideChar( cp, 0, src, read, NULL, 0 );
        }
        if (!(text = malloc( (len + 1) * sizeof(WCHAR) )))
        {
            free( data );
            return NULL;
        }
        MultiByteToWideChar( cp, 0, src, read, text, len );
        text[len] = 0;
    }
    free( data );
    return text;
}

int __cdecl wmain(int argc, WCHAR *argv[])
{
    struct script script = { 0 };
    int i;

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
            /* Parameter form: the shell already split the command into
             * words (powershell -Command Start-Process -FilePath <file> ...). */
            if (is_start_process( strip_quotes( argv[i + 1] ) ))
                run_command( &script, argc - i - 1, argv + i + 1 );
            /* Inline form: the whole script sits in one argument. */
            else
                run_script( &script, argv[i + 1] );
            return script_result( &script );
        }
        if (!wcsicmp( argv[i], L"-File" ) && i + 1 < argc)
        {
            WCHAR *text = read_script( argv[i + 1] );

            if (!text) return 1;
            run_script( &script, text );
            free( text );
            return script_result( &script );
        }
        if (argv[i][0] != '-')
        {
            /* A bare command: powershell Start-Process -FilePath <file> ... */
            run_command( &script, argc - i, argv + i );
            return script_result( &script );
        }
        /* -EncodedCommand, unknown switches: cannot run what follows */
        return 1;
    }
    return 0;
}
