/*
 * Copyright 2021 Florian Eder
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

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>
#include <wctype.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(robocopy);

/* bits of the exit code */
#define EXIT_COPIED   1
#define EXIT_EXTRA    2
#define EXIT_MISMATCH 4
#define EXIT_FAILED   8
#define EXIT_FATAL    16

struct name_list
{
    WCHAR **names;
    unsigned int count;
    unsigned int capacity;
};

struct entry
{
    WCHAR *name;
    DWORD attrs;
    ULONGLONG size;
    FILETIME mtime;
};

struct listing
{
    struct entry *entries;
    unsigned int count;
    unsigned int capacity;
};

struct counters
{
    ULONGLONG total;
    ULONGLONG copied;
    ULONGLONG mismatch;
    ULONGLONG failed;
    ULONGLONG extras;
};

static struct
{
    WCHAR *source;
    WCHAR *dest;
    struct name_list files;
    struct name_list exclude_files;
    struct name_list exclude_dirs;
    BOOL subdirs;         /* /S */
    BOOL empty_subdirs;   /* /E */
    BOOL purge;           /* /PURGE */
    BOOL mirror;          /* /MIR */
    BOOL move_files;      /* /MOV */
    BOOL move_dirs;       /* /MOVE */
    BOOL exclude_older;   /* /XO */
    BOOL no_size;         /* /NS */
    BOOL no_class;        /* /NC */
    BOOL no_file_list;    /* /NFL */
    BOOL no_dir_list;     /* /NDL */
    BOOL no_progress;     /* /NP */
    BOOL no_summary;      /* /NJS */
    BOOL no_header;       /* /NJH */
    unsigned int retries; /* /R:n */
    unsigned int wait;    /* /W:n */
} options;

static struct counters dir_stats, file_stats, byte_stats;
static int exit_code;

static void output( const WCHAR *format, ... )
{
    va_list args;
    WCHAR *buffer;
    DWORD count;
    int len;

    va_start( args, format );
    len = _vscwprintf( format, args );
    va_end( args );
    if (len <= 0 || !(buffer = malloc( (len + 1) * sizeof(WCHAR) ))) return;
    va_start( args, format );
    _vsnwprintf( buffer, len + 1, format, args );
    va_end( args );

    if (!WriteConsoleW( GetStdHandle( STD_OUTPUT_HANDLE ), buffer, len, &count, NULL ))
    {
        /* WriteConsoleW() fails if the output is redirected: write OEM text instead */
        int size = WideCharToMultiByte( GetOEMCP(), 0, buffer, len, NULL, 0, NULL, NULL );
        char *text = malloc( size );

        if (text)
        {
            WideCharToMultiByte( GetOEMCP(), 0, buffer, len, text, size, NULL, NULL );
            WriteFile( GetStdHandle( STD_OUTPUT_HANDLE ), text, size, &count, NULL );
            free( text );
        }
    }
    free( buffer );
}

static void output_error( DWORD error, const WCHAR *action, const WCHAR *path )
{
    WCHAR message[512];
    SYSTEMTIME now;

    GetLocalTime( &now );
    message[0] = 0;
    FormatMessageW( FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, error, 0,
                    message, ARRAY_SIZE(message), NULL );
    output( L"%04u/%02u/%02u %02u:%02u:%02u ERROR %lu (0x%08lX) %ls %ls\r\n%ls\r\n",
            now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
            error, error, action, path, message );
}

static void add_name( struct name_list *list, const WCHAR *name )
{
    if (list->count == list->capacity)
    {
        list->capacity = list->capacity ? list->capacity * 2 : 8;
        list->names = realloc( list->names, list->capacity * sizeof(*list->names) );
    }
    list->names[list->count++] = wcsdup( name );
}

static WCHAR *get_full_path( const WCHAR *path )
{
    DWORD len = GetFullPathNameW( path, 0, NULL, NULL );
    WCHAR *full;

    if (!len) return wcsdup( path );
    full = malloc( len * sizeof(WCHAR) );
    len = GetFullPathNameW( path, len, full, NULL );
    /* no trailing backslash, except for the root of a drive */
    while (len > 3 && (full[len - 1] == '\\' || full[len - 1] == '/')) full[--len] = 0;
    return full;
}

static WCHAR *join_path( const WCHAR *dir, const WCHAR *name )
{
    size_t dir_len = wcslen( dir ), name_len = wcslen( name );
    WCHAR *path = malloc( (dir_len + name_len + 2) * sizeof(WCHAR) );

    memcpy( path, dir, dir_len * sizeof(WCHAR) );
    if (dir_len && dir[dir_len - 1] != '\\') path[dir_len++] = '\\';
    memcpy( path + dir_len, name, (name_len + 1) * sizeof(WCHAR) );
    return path;
}

static BOOL match_pattern( const WCHAR *pattern, const WCHAR *name )
{
    while (*pattern)
    {
        if (*pattern == '*')
        {
            while (*pattern == '*') pattern++;
            if (!*pattern) return TRUE;
            for (; *name; name++) if (match_pattern( pattern, name )) return TRUE;
            return FALSE;
        }
        if (!*name) return FALSE;
        if (*pattern != '?' && towlower( *pattern ) != towlower( *name )) return FALSE;
        pattern++;
        name++;
    }
    return !*name;
}

/* A name in a list is a wildcard pattern for the last path component, or, with a
 * path separator in it, a full path that has to be equal to one of the two paths. */
static BOOL match_list( const struct name_list *list, const WCHAR *name,
                        const WCHAR *path1, const WCHAR *path2 )
{
    unsigned int i;

    for (i = 0; i < list->count; i++)
    {
        const WCHAR *pattern = list->names[i];

        if (wcspbrk( pattern, L"\\/:" ))
        {
            WCHAR *full = get_full_path( pattern );
            BOOL ret = (path1 && !wcsicmp( full, path1 )) || (path2 && !wcsicmp( full, path2 ));

            free( full );
            if (ret) return TRUE;
        }
        else if (!wcscmp( pattern, L"*.*" ) || match_pattern( pattern, name )) return TRUE;
    }
    return FALSE;
}

static BOOL is_included_file( const WCHAR *name )
{
    return !options.files.count || match_list( &options.files, name, NULL, NULL );
}

static int __cdecl compare_entries( const void *a, const void *b )
{
    return wcsicmp( ((const struct entry *)a)->name, ((const struct entry *)b)->name );
}

static BOOL read_listing( const WCHAR *dir, struct listing *list )
{
    WCHAR *pattern = join_path( dir, L"*" );
    WIN32_FIND_DATAW data;
    HANDLE find;

    memset( list, 0, sizeof(*list) );
    find = FindFirstFileW( pattern, &data );
    free( pattern );
    if (find == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;

    do
    {
        struct entry *entry;

        if (!wcscmp( data.cFileName, L"." ) || !wcscmp( data.cFileName, L".." )) continue;
        if (list->count == list->capacity)
        {
            list->capacity = list->capacity ? list->capacity * 2 : 32;
            list->entries = realloc( list->entries, list->capacity * sizeof(*list->entries) );
        }
        entry = &list->entries[list->count++];
        entry->name = wcsdup( data.cFileName );
        entry->attrs = data.dwFileAttributes;
        entry->size = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        entry->mtime = data.ftLastWriteTime;
    } while (FindNextFileW( find, &data ));
    FindClose( find );

    qsort( list->entries, list->count, sizeof(*list->entries), compare_entries );
    return TRUE;
}

static void free_listing( struct listing *list )
{
    unsigned int i;

    for (i = 0; i < list->count; i++) free( list->entries[i].name );
    free( list->entries );
}

static const struct entry *find_entry( const struct listing *list, const WCHAR *name )
{
    struct entry key;

    if (!list->count) return NULL;
    key.name = (WCHAR *)name;
    return bsearch( &key, list->entries, list->count, sizeof(*list->entries), compare_entries );
}

static BOOL is_directory( const WCHAR *path )
{
    DWORD attrs = GetFileAttributesW( path );

    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

/* create a directory and the missing ones above it */
static BOOL create_directory( const WCHAR *path )
{
    WCHAR *parent, *p;
    BOOL ret;

    if (is_directory( path )) return TRUE;
    if (CreateDirectoryW( path, NULL )) return TRUE;
    if (GetLastError() != ERROR_PATH_NOT_FOUND) return FALSE;

    parent = wcsdup( path );
    if (!(p = wcsrchr( parent, '\\' )) || p == parent || p[-1] == ':')
    {
        free( parent );
        SetLastError( ERROR_PATH_NOT_FOUND );
        return FALSE;
    }
    *p = 0;
    ret = create_directory( parent );
    free( parent );
    return ret && CreateDirectoryW( path, NULL );
}

static void output_dir_line( const WCHAR *class, int count, const WCHAR *path )
{
    if (options.no_dir_list) return;
    output( L"\t%-12ls%8d\t%ls\\\r\n", options.no_class ? L"" : class, count, path );
}

static void output_file_line( const WCHAR *class, ULONGLONG size, const WCHAR *dir, const WCHAR *name,
                              BOOL always )
{
    WCHAR size_str[32] = L"";

    if (options.no_file_list && !always) return;
    if (!options.no_size) swprintf( size_str, ARRAY_SIZE(size_str), L"%I64u", size );
    output( L"\t%14ls\t\t%8ls\t", options.no_class ? L"" : class, size_str );
    /* without the directory lines a file is shown with its path */
    if (options.no_dir_list) output( L"%ls\\%ls", dir, name );
    else output( L"%ls", name );
}

static void delete_file( const WCHAR *path, DWORD attrs )
{
    if (attrs & FILE_ATTRIBUTE_READONLY) SetFileAttributesW( path, attrs & ~FILE_ATTRIBUTE_READONLY );
    if (!DeleteFileW( path ))
    {
        output_error( GetLastError(), L"Deleting Extra File", path );
        exit_code |= EXIT_FAILED;
    }
}

/* remove a directory of the destination that the source does not have */
static void delete_extra_directory( const WCHAR *path, DWORD attrs )
{
    struct listing list;
    unsigned int i;

    /* a link is removed, not what it points to */
    if (!(attrs & FILE_ATTRIBUTE_REPARSE_POINT) && read_listing( path, &list ))
    {
        for (i = 0; i < list.count; i++)
        {
            const struct entry *entry = &list.entries[i];
            WCHAR *sub = join_path( path, entry->name );

            if (entry->attrs & FILE_ATTRIBUTE_DIRECTORY)
            {
                dir_stats.extras++;
                delete_extra_directory( sub, entry->attrs );
            }
            else
            {
                file_stats.extras++;
                byte_stats.extras += entry->size;
                output_file_line( L"*EXTRA File", entry->size, path, entry->name, TRUE );
                output( L"\r\n" );
                delete_file( sub, entry->attrs );
            }
            free( sub );
        }
        free_listing( &list );
    }
    if (attrs & FILE_ATTRIBUTE_READONLY) SetFileAttributesW( path, attrs & ~FILE_ATTRIBUTE_READONLY );
    if (!RemoveDirectoryW( path ))
    {
        output_error( GetLastError(), L"Deleting Extra Directory", path );
        exit_code |= EXIT_FAILED;
    }
}

/* Find the files and directories of the destination that the source does not have,
 * and remove them with /PURGE. An entry of the other type (a file where the source
 * has a directory, or the reverse) is in the way of the copy: it is removed with
 * /PURGE and reported as a mismatch by the copy otherwise. */
static void process_extras( const struct listing *src_list, const WCHAR *dst )
{
    struct listing dst_list;
    unsigned int i;

    if (!read_listing( dst, &dst_list )) return;
    for (i = 0; i < dst_list.count; i++)
    {
        const struct entry *entry = &dst_list.entries[i];
        const struct entry *src_entry = find_entry( src_list, entry->name );
        BOOL is_dir = !!(entry->attrs & FILE_ATTRIBUTE_DIRECTORY);
        WCHAR *path;

        if (src_entry)
        {
            if (is_dir == !!(src_entry->attrs & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (!options.purge) continue;
        }
        path = join_path( dst, entry->name );
        if (is_dir)
        {
            dir_stats.extras++;
            exit_code |= EXIT_EXTRA;
            /* an excluded directory counts as extra and stays */
            if (!match_list( &options.exclude_dirs, entry->name, path, NULL ))
            {
                /* extra entries are listed with /NDL and /NFL as well */
                output( L"\t%-12ls%8d\t%ls\\\r\n", options.no_class ? L"" : L"  *EXTRA Dir", -1, path );
                if (options.purge) delete_extra_directory( path, entry->attrs );
            }
        }
        else if (is_included_file( entry->name ))
        {
            file_stats.extras++;
            exit_code |= EXIT_EXTRA;
            /* the same for an excluded file */
            if (!match_list( &options.exclude_files, entry->name, path, NULL ))
            {
                byte_stats.extras += entry->size;
                output_file_line( L"*EXTRA File", entry->size, dst, entry->name, TRUE );
                output( L"\r\n" );
                if (options.purge) delete_file( path, entry->attrs );
            }
        }
        free( path );
    }
    free_listing( &dst_list );
}

static BOOL copy_file( const WCHAR *src, const WCHAR *dst, DWORD dst_attrs )
{
    unsigned int attempt;
    DWORD error;

    for (attempt = 0; ; attempt++)
    {
        /* CopyFileW() does not replace a read-only or hidden file */
        if (dst_attrs != INVALID_FILE_ATTRIBUTES &&
            (dst_attrs & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
            SetFileAttributesW( dst, FILE_ATTRIBUTE_NORMAL );
        if (CopyFileW( src, dst, FALSE )) return TRUE;

        error = GetLastError();
        output( L"\r\n" );
        output_error( error, L"Copying File", src );
        if (attempt >= options.retries)
        {
            output( L"ERROR: RETRY LIMIT EXCEEDED.\r\n\r\n" );
            return FALSE;
        }
        output( L"Waiting %u seconds... Retrying...\r\n", options.wait );
        Sleep( options.wait * 1000 );
    }
}

static void process_file( const struct entry *entry, const WCHAR *src_dir, const WCHAR *dst_dir,
                          BOOL *dst_created )
{
    WCHAR *src = join_path( src_dir, entry->name ), *dst = join_path( dst_dir, entry->name );
    WIN32_FILE_ATTRIBUTE_DATA info;
    const WCHAR *class = L"New File";
    DWORD dst_attrs = INVALID_FILE_ATTRIBUTES;

    file_stats.total++;
    byte_stats.total += entry->size;

    if (match_list( &options.exclude_files, entry->name, src, dst )) goto done;

    if (GetFileAttributesExW( dst, GetFileExInfoStandard, &info ))
    {
        ULONGLONG size = ((ULONGLONG)info.nFileSizeHigh << 32) | info.nFileSizeLow;
        int cmp = CompareFileTime( &entry->mtime, &info.ftLastWriteTime );

        dst_attrs = info.dwFileAttributes;
        if (dst_attrs & FILE_ATTRIBUTE_DIRECTORY)
        {
            output_file_line( L"*MISMATCH", entry->size, src_dir, entry->name, FALSE );
            if (!options.no_file_list) output( L"\r\n" );
            file_stats.mismatch++;
            byte_stats.mismatch += entry->size;
            exit_code |= EXIT_MISMATCH;
            goto done;
        }
        if (!cmp && size == entry->size) goto done;
        if (cmp < 0 && options.exclude_older) goto done;
        class = cmp > 0 ? L"Newer" : cmp < 0 ? L"Older" : L"Changed";
    }

    if (!*dst_created)
    {
        /* with /S a directory appears with its first file */
        if (!create_directory( dst_dir ))
        {
            output_error( GetLastError(), L"Creating Destination Directory", dst_dir );
            file_stats.failed++;
            byte_stats.failed += entry->size;
            exit_code |= EXIT_FAILED;
            goto done;
        }
        *dst_created = TRUE;
    }

    output_file_line( class, entry->size, src_dir, entry->name, FALSE );
    if (copy_file( src, dst, dst_attrs ))
    {
        if (!options.no_file_list) output( options.no_progress ? L"\r\n" : L"\r100%%  \r\n" );
        /* the copy has the attributes of the source */
        if (entry->attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
            SetFileAttributesW( dst, entry->attrs );
        file_stats.copied++;
        byte_stats.copied += entry->size;
        exit_code |= EXIT_COPIED;
        /* only a file that was copied is removed from the source */
        if (options.move_files)
        {
            if (entry->attrs & FILE_ATTRIBUTE_READONLY)
                SetFileAttributesW( src, entry->attrs & ~FILE_ATTRIBUTE_READONLY );
            if (!DeleteFileW( src )) output_error( GetLastError(), L"Deleting Source File", src );
        }
    }
    else
    {
        file_stats.failed++;
        byte_stats.failed += entry->size;
        exit_code |= EXIT_FAILED;
    }

done:
    free( src );
    free( dst );
}

static void process_directory( const WCHAR *src, const WCHAR *dst, DWORD src_attrs, BOOL is_root )
{
    struct listing src_list;
    BOOL dst_exists, dst_created;
    unsigned int i;
    int file_count = 0;

    if (!read_listing( src, &src_list ))
    {
        output_error( GetLastError(), L"Accessing Source Directory", src );
        dir_stats.failed++;
        exit_code |= EXIT_FAILED;
        return;
    }
    dir_stats.total++;

    for (i = 0; i < src_list.count; i++)
    {
        const struct entry *entry = &src_list.entries[i];

        if (!(entry->attrs & FILE_ATTRIBUTE_DIRECTORY) && is_included_file( entry->name )) file_count++;
    }

    dst_exists = dst_created = is_directory( dst );
    output_dir_line( dst_exists ? L"" : L"  New Dir", file_count, src );
    if (dst_exists) process_extras( &src_list, dst );
    else if (is_root || options.empty_subdirs)
    {
        if (!create_directory( dst ))
        {
            output_error( GetLastError(), L"Creating Destination Directory", dst );
            dir_stats.failed++;
            exit_code |= is_root ? EXIT_FATAL : EXIT_FAILED;
            free_listing( &src_list );
            return;
        }
        /* a new directory gets the attributes of the source */
        if (!is_root && (src_attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY)))
            SetFileAttributesW( dst, src_attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                                                    FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY) );
        dst_created = TRUE;
        dir_stats.copied++;
    }

    for (i = 0; i < src_list.count; i++)
    {
        const struct entry *entry = &src_list.entries[i];

        if (entry->attrs & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!is_included_file( entry->name )) continue;
        process_file( entry, src, dst, &dst_created );
    }

    for (i = 0; options.subdirs && i < src_list.count; i++)
    {
        const struct entry *entry = &src_list.entries[i];
        WCHAR *sub_src, *sub_dst;
        DWORD attrs;

        if (!(entry->attrs & FILE_ATTRIBUTE_DIRECTORY)) continue;
        sub_src = join_path( src, entry->name );
        sub_dst = join_path( dst, entry->name );
        if (!match_list( &options.exclude_dirs, entry->name, sub_src, sub_dst ))
        {
            attrs = GetFileAttributesW( sub_dst );
            if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
            {
                /* a file is in the way */
                output_dir_line( L"*MISMATCH", -1, sub_src );
                dir_stats.total++;
                dir_stats.mismatch++;
                exit_code |= EXIT_MISMATCH;
            }
            else process_directory( sub_src, sub_dst, entry->attrs, FALSE );
        }
        free( sub_src );
        free( sub_dst );
    }

    free_listing( &src_list );
    /* fails unless everything in it was moved */
    if (options.move_dirs) RemoveDirectoryW( src );
}

static void format_time( WCHAR *buffer, unsigned int size )
{
    WCHAR date[128], time[64];

    date[0] = time[0] = 0;
    GetDateFormatW( LOCALE_USER_DEFAULT, DATE_LONGDATE, NULL, NULL, date, ARRAY_SIZE(date) );
    GetTimeFormatW( LOCALE_USER_DEFAULT, 0, NULL, NULL, time, ARRAY_SIZE(time) );
    swprintf( buffer, size, L"%ls %ls", date, time );
}

static void output_header(void)
{
    WCHAR started[200];
    unsigned int i;

    format_time( started, ARRAY_SIZE(started) );
    output( L"-------------------------------------------------------------------------------\r\n"
            L"   ROBOCOPY     ::     Robust File Copy for Windows                              \r\n"
            L"-------------------------------------------------------------------------------\r\n\r\n" );
    output( L"  Started : %ls\r\n", started );
    output( L"   Source : %ls\\\r\n", options.source ? options.source : L"" );
    output( L"     Dest : %ls\\\r\n\r\n", options.dest ? options.dest : L"" );
    output( L"    Files : " );
    if (!options.files.count) output( L"*.*" );
    for (i = 0; i < options.files.count; i++) output( i ? L"\r\n\t    %ls" : L"%ls", options.files.names[i] );
    output( L"\r\n\t    \r\n" );
    if (options.exclude_files.count)
    {
        output( L" Exc Files : " );
        for (i = 0; i < options.exclude_files.count; i++)
            output( i ? L"\r\n\t     %ls" : L"%ls", options.exclude_files.names[i] );
        output( L"\r\n\t    \r\n" );
    }
    if (options.exclude_dirs.count)
    {
        output( L"  Exc Dirs : " );
        for (i = 0; i < options.exclude_dirs.count; i++)
            output( i ? L"\r\n\t     %ls" : L"%ls", options.exclude_dirs.names[i] );
        output( L"\r\n\t    \r\n" );
    }
    output( L"  Options : %ls", options.files.count ? L"" : L"*.* " );
    if (options.no_size) output( L"/NS " );
    if (options.no_class) output( L"/NC " );
    if (options.no_dir_list) output( L"/NDL " );
    if (options.no_file_list) output( L"/NFL " );
    if (options.no_summary) output( L"/NJS " );
    if (options.subdirs) output( L"/S " );
    if (options.empty_subdirs) output( L"/E " );
    output( L"/DCOPY:DA /COPY:DAT " );
    if (options.purge) output( L"/PURGE " );
    if (options.mirror) output( L"/MIR " );
    if (options.move_dirs) output( L"/MOVE " );
    else if (options.move_files) output( L"/MOV " );
    if (options.no_progress) output( L"/NP " );
    if (options.exclude_older) output( L"/XO " );
    output( L"/R:%u /W:%u \r\n\r\n", options.retries, options.wait );
    output( L"------------------------------------------------------------------------------\r\n\r\n" );
}

static void output_counters( const WCHAR *label, const struct counters *counters )
{
    output( L"%ls %9I64u %9I64u %9I64u %9I64u %9I64u %9I64u\r\n", label, counters->total, counters->copied,
            counters->total - counters->copied - counters->mismatch - counters->failed,
            counters->mismatch, counters->failed, counters->extras );
}

static void output_summary( ULONGLONG start )
{
    unsigned int seconds = (GetTickCount64() - start) / 1000;
    WCHAR ended[200];

    format_time( ended, ARRAY_SIZE(ended) );
    output( L"\r\n------------------------------------------------------------------------------\r\n\r\n" );
    output( L"               Total    Copied   Skipped  Mismatch    FAILED    Extras\r\n" );
    output_counters( L"    Dirs :", &dir_stats );
    output_counters( L"   Files :", &file_stats );
    output_counters( L"   Bytes :", &byte_stats );
    output( L"   Times :%4u:%02u:%02u   0:00:00                       0:00:00   0:00:00\r\n",
            seconds / 3600, seconds / 60 % 60, seconds % 60 );
    output( L"   Ended : %ls\r\n\r\n", ended );
}

static void output_usage(void)
{
    output( L"       Simple Usage :: ROBOCOPY source destination /MIR\r\n\r\n"
            L"             source :: Source Directory (drive:\\path or \\\\server\\share\\path).\r\n"
            L"        destination :: Destination Dir  (drive:\\path or \\\\server\\share\\path).\r\n"
            L"               /MIR :: Mirror a complete directory tree.\r\n\r\n"
            L"    Supported options: /S /E /PURGE /MIR /MOV /MOVE /XO /XF file... /XD dir...\r\n"
            L"                       /R:n /W:n /NS /NC /NFL /NDL /NP /NJS /NJH\r\n\r\n"
            L"****  /MIR can DELETE files as well as copy them !\r\n" );
}

static BOOL parse_number( const WCHAR *str, unsigned int *value )
{
    WCHAR *end;
    unsigned long number = wcstoul( str, &end, 10 );

    /* something that is not a number leaves the default */
    if (end != str && !*end) *value = number;
    return TRUE;
}

static BOOL parse_option( const WCHAR *arg, struct name_list **list )
{
    static const struct
    {
        const WCHAR *name;
        BOOL *flag;
    }
    flags[] =
    {
        { L"/xo",  &options.exclude_older },
        { L"/ns",  &options.no_size },
        { L"/nc",  &options.no_class },
        { L"/nfl", &options.no_file_list },
        { L"/ndl", &options.no_dir_list },
        { L"/np",  &options.no_progress },
        { L"/njs", &options.no_summary },
        { L"/njh", &options.no_header },
    };
    unsigned int i;

    *list = NULL;
    for (i = 0; i < ARRAY_SIZE(flags); i++)
    {
        if (wcsicmp( arg, flags[i].name )) continue;
        *flags[i].flag = TRUE;
        return TRUE;
    }
    if (!wcsicmp( arg, L"/s" )) options.subdirs = TRUE;
    else if (!wcsicmp( arg, L"/e" )) options.subdirs = options.empty_subdirs = TRUE;
    else if (!wcsicmp( arg, L"/purge" )) options.purge = TRUE;
    else if (!wcsicmp( arg, L"/mir" ))
        options.subdirs = options.empty_subdirs = options.purge = options.mirror = TRUE;
    else if (!wcsicmp( arg, L"/mov" )) options.move_files = TRUE;
    else if (!wcsicmp( arg, L"/move" )) options.move_files = options.move_dirs = TRUE;
    else if (!wcsicmp( arg, L"/xf" )) *list = &options.exclude_files;
    else if (!wcsicmp( arg, L"/xd" )) *list = &options.exclude_dirs;
    else if (!wcsnicmp( arg, L"/r:", 3 )) return parse_number( arg + 3, &options.retries );
    else if (!wcsnicmp( arg, L"/w:", 3 )) return parse_number( arg + 3, &options.wait );
    else return FALSE;
    return TRUE;
}

int __cdecl wmain( int argc, WCHAR *argv[] )
{
    struct name_list *list = NULL;
    const WCHAR *invalid = NULL;
    ULONGLONG start = GetTickCount64();
    DWORD attrs;
    int i, invalid_index = 0;

    options.retries = 1000000;
    options.wait = 30;

    for (i = 1; i < argc; i++)
    {
        TRACE( "argument %d: %s\n", i, debugstr_w(argv[i]) );
        if (argv[i][0] == '/')
        {
            if (!parse_option( argv[i], &list ) && !invalid)
            {
                invalid = argv[i];
                invalid_index = i;
            }
        }
        else if (list) add_name( list, argv[i] );
        else if (!options.source) options.source = get_full_path( argv[i] );
        else if (!options.dest) options.dest = get_full_path( argv[i] );
        else add_name( &options.files, argv[i] );
    }

    output( L"\r\n" );
    if (invalid || !options.source || !options.dest)
    {
        if (options.source) output_header();
        if (invalid && wcscmp( invalid, L"/?" ))
        {
            WARN( "unsupported option %s\n", debugstr_w(invalid) );
            output( L"ERROR : Invalid Parameter #%d : \"%ls\"\r\n\r\n", invalid_index, invalid );
        }
        else if (!invalid && options.source)
            output( L"ERROR : No Destination Directory Specified.\r\n\r\n" );
        output_usage();
        return EXIT_FATAL;
    }

    if (!options.no_header) output_header();

    attrs = GetFileAttributesW( options.source );
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
    {
        output_error( attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY,
                      L"Accessing Source Directory", options.source );
        return EXIT_FATAL;
    }

    process_directory( options.source, options.dest, attrs, TRUE );

    if (!options.no_summary) output_summary( start );
    return exit_code;
}
