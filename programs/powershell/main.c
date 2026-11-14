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
 * the child's exit code is that result.
 *
 * It also answers version queries -- $PSVersionTable.PSVersion, $Host.Version
 * and (Get-Host).Version with their members, string and number literals and
 * the -f format operator -- by writing the value to stdout the way
 * PowerShell 5.1 formats it.  Applications check the version before they use
 * PowerShell at all.  A script that only produces values succeeds only if
 * every statement in it was understood. */

/* The PowerShell that ships with Windows 10 22H2 (19045.6456). */
static const LONG ps_version[4] = { 5, 1, 19041, 6456 };

struct script
{
    HANDLE procs[64];       /* started but not yet awaited */
    unsigned int count;
    DWORD failure;          /* exit code of the first awaited process that failed */
    BOOL started;           /* at least one process was started */
    BOOL printed;           /* at least one value was written to stdout */
    BOOL skipped;           /* at least one statement was not executed */
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

enum value_type { VAL_NUMBER, VAL_STRING, VAL_VERSION };

struct value
{
    enum value_type type;
    LONG number;
    WCHAR string[256];
};

static const WCHAR *skip_space( const WCHAR *p )
{
    while (is_space( *p )) p++;
    return p;
}

/* A case-insensitive identifier that is not the start of a longer one. */
static BOOL match_word( const WCHAR **p, const WCHAR *word )
{
    size_t len = wcslen( word );
    WCHAR next = (*p)[len];

    if (wcsnicmp( *p, word, len )) return FALSE;
    if (iswalnum( next ) || next == '_' || next == '-') return FALSE;
    *p += len;
    return TRUE;
}

/* A System.Version, or one of its members after a '.'. */
static BOOL parse_version_members( const WCHAR **p, struct value *v )
{
    static const WCHAR *fields[] = { L"Major", L"Minor", L"Build", L"Revision" };
    unsigned int i;

    v->type = VAL_VERSION;
    if (**p != '.') return TRUE;
    (*p)++;
    for (i = 0; i < ARRAY_SIZE(fields); i++)
    {
        if (!match_word( p, fields[i] )) continue;
        v->type = VAL_NUMBER;
        v->number = ps_version[i];
        return TRUE;
    }
    if (match_word( p, L"ToString" ) && (*p)[0] == '(' && (*p)[1] == ')')
    {
        *p += 2;
        v->type = VAL_STRING;
        swprintf( v->string, ARRAY_SIZE(v->string), L"%ld.%ld.%ld.%ld",
                  ps_version[0], ps_version[1], ps_version[2], ps_version[3] );
        return TRUE;
    }
    return FALSE;
}

/* A quoted string, a decimal number, or one of the version values. */
static BOOL parse_operand( const WCHAR **p, struct value *v )
{
    const WCHAR *s = *p;
    unsigned int len = 0;

    if (*s == '\'' || *s == '"')
    {
        WCHAR quote = *s++;

        v->type = VAL_STRING;
        for (;;)
        {
            if (!*s) return FALSE;
            if (*s == quote)
            {
                if (s[1] != quote) break;
                s++;    /* a doubled quote is a literal one */
            }
            /* no variable expansion or escapes in double-quoted strings */
            else if (quote == '"' && (*s == '$' || *s == '`')) return FALSE;
            if (len == ARRAY_SIZE(v->string) - 1) return FALSE;
            v->string[len++] = *s++;
        }
        v->string[len] = 0;
        s++;
    }
    else if (iswdigit( *s ))
    {
        v->type = VAL_NUMBER;
        v->number = 0;
        while (iswdigit( *s ))
        {
            if (v->number > 100000000) return FALSE;
            v->number = v->number * 10 + (*s++ - '0');
        }
        if (iswalnum( *s ) || *s == '.') return FALSE;
    }
    else if (*s == '$')
    {
        s++;
        if (match_word( &s, L"PSVersionTable" ))
        {
            if (*s++ != '.' || !match_word( &s, L"PSVersion" )) return FALSE;
        }
        else if (match_word( &s, L"Host" ))
        {
            if (*s++ != '.' || !match_word( &s, L"Version" )) return FALSE;
        }
        else return FALSE;
        if (!parse_version_members( &s, v )) return FALSE;
    }
    else if (!wcsnicmp( s, L"(Get-Host).", 11 ))
    {
        s += 11;
        if (!match_word( &s, L"Version" ) || !parse_version_members( &s, v )) return FALSE;
    }
    else return FALSE;

    *p = s;
    return TRUE;
}

static void value_to_string( const struct value *v, WCHAR *buf, size_t size )
{
    switch (v->type)
    {
    case VAL_NUMBER:
        swprintf( buf, size, L"%ld", v->number );
        break;
    case VAL_STRING:
        lstrcpynW( buf, v->string, size );
        break;
    case VAL_VERSION:
        swprintf( buf, size, L"%ld.%ld.%ld.%ld",
                  ps_version[0], ps_version[1], ps_version[2], ps_version[3] );
        break;
    }
}

/* The -f operator with plain {N} items; alignment and format strings are not
 * supported. */
static BOOL format_string( struct value *v, const struct value *args, unsigned int count )
{
    WCHAR result[256], arg[256];
    const WCHAR *f = v->string;
    size_t len = 0, n;

    while (*f)
    {
        if ((f[0] == '{' && f[1] == '{') || (f[0] == '}' && f[1] == '}'))
        {
            arg[0] = *f;
            arg[1] = 0;
            f += 2;
        }
        else if (*f == '{')
        {
            unsigned int index = 0;

            if (!iswdigit( *++f )) return FALSE;
            while (iswdigit( *f )) index = index * 10 + (*f++ - '0');
            if (*f++ != '}' || index >= count) return FALSE;
            value_to_string( &args[index], arg, ARRAY_SIZE(arg) );
        }
        else if (*f == '}') return FALSE;
        else
        {
            arg[0] = *f++;
            arg[1] = 0;
        }
        n = wcslen( arg );
        if (len + n >= ARRAY_SIZE(result)) return FALSE;
        memcpy( result + len, arg, n * sizeof(WCHAR) );
        len += n;
    }
    result[len] = 0;
    wcscpy( v->string, result );
    return TRUE;
}

/* A statement that is a value: an operand, optionally formatted with
 * "-f <operand>, <operand>, ...".  The whole statement must be consumed. */
static BOOL parse_expression( const WCHAR *text, struct value *v )
{
    struct value args[16];
    unsigned int count = 0;
    const WCHAR *p = skip_space( text );

    if (!parse_operand( &p, v )) return FALSE;
    p = skip_space( p );
    if (p[0] == '-' && (p[1] == 'f' || p[1] == 'F') && is_space( p[2] ))
    {
        if (v->type != VAL_STRING) return FALSE;
        p = skip_space( p + 2 );
        for (;;)
        {
            if (count == ARRAY_SIZE(args) || !parse_operand( &p, &args[count++] )) return FALSE;
            p = skip_space( p );
            if (*p != ',') break;
            p = skip_space( p + 1 );
        }
        if (!format_string( v, args, count )) return FALSE;
    }
    return !*p;
}

static void write_output( const WCHAR *text )
{
    HANDLE out = GetStdHandle( STD_OUTPUT_HANDLE );
    DWORD mode, written, len = wcslen( text );
    UINT cp;
    char *buf;
    int size;

    if (GetConsoleMode( out, &mode ))
    {
        WriteConsoleW( out, text, len, &written, NULL );
        return;
    }
    /* redirected output uses the console output code page, like PowerShell */
    if (!(cp = GetConsoleOutputCP())) cp = GetOEMCP();
    size = WideCharToMultiByte( cp, 0, text, len, NULL, 0, NULL, NULL );
    if (!(buf = malloc( size ))) return;
    WideCharToMultiByte( cp, 0, text, len, buf, size, NULL, NULL );
    WriteFile( out, buf, size, &written, NULL );
    free( buf );
}

/* PowerShell's default output formatting: numbers and strings on a line of
 * their own, a System.Version as a table. */
static void output_value( const struct value *v )
{
    WCHAR buf[512];

    if (v->type == VAL_VERSION)
        swprintf( buf, ARRAY_SIZE(buf),
                  L"\r\nMajor  Minor  Build  Revision\r\n"
                  L"-----  -----  -----  --------\r\n"
                  L"%-5ld  %-5ld  %-5ld  %-8ld\r\n\r\n\r\n",
                  ps_version[0], ps_version[1], ps_version[2], ps_version[3] );
    else
    {
        value_to_string( v, buf, ARRAY_SIZE(buf) - 2 );
        wcscat( buf, L"\r\n" );
    }
    write_output( buf );
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
    {
        if (start_process( s, argc, argv, i ) < 0) s->skipped = TRUE;
    }
    else if (!wcsicmp( cmd, L"Wait-Process" ))
        wait_all( s );
    else if (!wcsicmp( cmd, L"New-Item" ))
        new_item( argc, argv, i );
    else
    {
        WINE_TRACE( "skipping %s\n", wine_dbgstr_w(cmd) );
        s->skipped = TRUE;
    }
}

/* One statement: a value to write out, or commands split at unquoted '|'
 * into pipeline segments. */
static void run_statement( struct script *s, WCHAR *stmt )
{
    struct value value;
    WCHAR *tok[64];
    int n, i, start = 0;

    if (parse_expression( stmt, &value ))
    {
        WINE_TRACE( "value %s\n", wine_dbgstr_w(stmt) );
        output_value( &value );
        s->printed = TRUE;
        return;
    }

    n = tokenize( stmt, tok, ARRAY_SIZE(tok) );
    for (i = 0; i <= n; i++)
    {
        if (i < n && wcscmp( tok[i], L"|" )) continue;
        if (i > start) run_command( s, i - start, tok + start );
        start = i + 1;
    }
}

/* The body of a script that is a single "& { ... }" invocation, as in
 * powershell -Command "& { ... }"; NULL for anything else. */
static WCHAR *script_block_body( WCHAR *text )
{
    WCHAR *p = (WCHAR *)skip_space( text ), *body, quote = 0;
    int depth = 0;

    if (*p++ != '&') return NULL;
    p = (WCHAR *)skip_space( p );
    if (*p != '{') return NULL;
    body = p + 1;
    for (; *p; p++)
    {
        if (quote) { if (*p == quote) quote = 0; }
        else if (*p == '\'' || *p == '"') quote = *p;
        else if (*p == '{') depth++;
        else if (*p == '}' && !--depth) break;
    }
    if (!*p || *skip_space( p + 1 )) return NULL;
    *p = 0;
    return body;
}

/* Statements end at an unquoted ';' or newline; '#' starts a comment line. */
static void run_script( struct script *s, WCHAR *text )
{
    WCHAR *p = text, *stmt, *body;

    if ((body = script_block_body( text )))
    {
        run_script( s, body );
        return;
    }

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
    if (s->started) return s->failure;
    /* Values alone count only if nothing else in the script was skipped. */
    if (s->printed && !s->skipped) return 0;
    return 1;
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
        if (argv[i][0] && (wcschr( L"$'\"(&", argv[i][0] ) || iswdigit( argv[i][0] )))
        {
            /* A bare script that starts with a value or an invocation:
             * powershell $PSVersionTable.PSVersion.Major.  PowerShell joins
             * the remaining arguments with spaces. */
            WCHAR *text;
            size_t len = 0;
            int j;

            for (j = i; j < argc; j++) len += wcslen( argv[j] ) + 1;
            if (!(text = malloc( len * sizeof(WCHAR) ))) return 1;
            text[0] = 0;
            for (j = i; j < argc; j++)
            {
                if (j > i) wcscat( text, L" " );
                wcscat( text, argv[j] );
            }
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
