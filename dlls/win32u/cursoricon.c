/*
 * Cursor and icon support
 *
 * Copyright 1995 Alexandre Julliard
 * Copyright 1996 Martin Von Loewis
 * Copyright 1997 Alex Korobka
 * Copyright 1998 Turchanov Sergey
 * Copyright 2007 Henri Verbeet
 * Copyright 2009 Vincent Povirk for CodeWeavers
 * Copyright 2016 Dmitry Timoshkov
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

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ntgdi_private.h"
#include "ntuser_private.h"
#include "wine/server.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(cursor);
WINE_DECLARE_DEBUG_CHANNEL(icon);

struct cursoricon_object
{
    HICON                   handle;     /* cursor full handle */
    struct list             entry;      /* entry in shared icons list */
    struct free_icon_params params;     /* opaque params used by 16-bit code */
    UNICODE_STRING          module;     /* module for icons loaded from resources */
    WCHAR                  *resname;    /* resource name for icons loaded from resources */
    HRSRC                   rsrc;       /* resource for shared icons */
    HANDLE                  section;    /* section publishing the first frame to other processes */
    BOOL                    is_shared;  /* whether this object is shared */
    BOOL                    is_icon;    /* whether icon or cursor */
    BOOL                    is_ani;     /* whether this object is a static cursor or an animated cursor */
    UINT                    delay;      /* delay between this frame and the next (in jiffies) */
    union
    {
        struct cursoricon_frame  frame; /* frame-specific icon data */
        struct
        {
            UINT  num_frames;           /* number of frames in the icon/cursor */
            UINT  num_steps;            /* number of sequence steps in the icon/cursor */
            HICON *frames;              /* list of animated cursor frames */
        } ani;
    };
};

static struct list icon_cache = LIST_INIT( icon_cache );

static struct cursoricon_object *get_icon_ptr( HICON handle )
{
    struct cursoricon_object *obj = get_user_handle_ptr( handle, NTUSER_OBJ_ICON );
    if (obj == OBJ_OTHER_PROCESS)
    {
        WARN( "icon handle %p from other process\n", handle );
        obj = NULL;
    }
    return obj;
}

static void publish_cursor( HCURSOR handle );
static HCURSOR get_foreign_cursor( HCURSOR foreign );

BOOL process_wine_setcursor( HWND hwnd, HWND window, HCURSOR handle )
{
    TRACE( "hwnd %p, window %p, hcursor %p\n", hwnd, window, handle );
    if (!handle)
    {
        /* Native Windows shows the class cursor, then the system arrow, for a
         * window whose cursor is not set — never the empty/transparent cursor
         * that hides the pointer. Only ShowCursor(FALSE) (count < 0) should
         * hide it; CURSOR_SHOWING is clear in that case. */
        CURSORINFO info;
        info.cbSize = sizeof(info);
        if (NtUserGetCursorInfo( &info ) && (info.flags & CURSOR_SHOWING))
        {
            handle = (HCURSOR)get_class_long_ptr( window, GCLP_HCURSOR, FALSE );
            if (!handle) handle = LoadImageW( 0, MAKEINTRESOURCEW(IDC_ARROW),
                                              IMAGE_CURSOR, 0, 0,
                                              LR_SHARED | LR_DEFAULTSIZE );
        }
    }
    if (handle)
    {
        /* The server routes this message to the thread owning the window under
         * the pointer; the cursor may have been created by another process
         * (out-of-process child windows). Use a local proxy for it then. */
        struct cursoricon_object *obj = get_user_handle_ptr( handle, NTUSER_OBJ_ICON );
        if (obj == OBJ_OTHER_PROCESS)
        {
            HCURSOR proxy = get_foreign_cursor( handle );
            TRACE( "cursor %p from other process -> proxy %p\n", handle, proxy );
            if (proxy) handle = proxy;
        }
        else if (obj) release_user_handle_ptr( obj );
    }
    user_driver->pSetCursor( window, handle );
    return TRUE;
}

/***********************************************************************
 *	     NtUserShowCursor    (win32u.@)
 */
INT WINAPI NtUserShowCursor( BOOL show )
{
    int increment = show ? 1 : -1;
    int count;

    SERVER_START_REQ( set_cursor )
    {
        req->flags = SET_CURSOR_COUNT;
        req->show_count = increment;
        wine_server_call( req );
        count = reply->prev_count + increment;
    }
    SERVER_END_REQ;

    TRACE("%d, count=%d\n", show, count );
    return count;
}

/***********************************************************************
 *	     NtUserSetCursor (win32u.@)
 */
HCURSOR WINAPI NtUserSetCursor( HCURSOR cursor )
{
    struct cursoricon_object *obj;
    HCURSOR old_cursor;
    BOOL ret;

    TRACE( "%p\n", cursor );

    /* cursors are published on creation; this covers icons used as cursors and
     * must happen before the server request, as the receiving thread may process
     * the resulting WM_WINE_SETCURSOR before this call returns */
    if (cursor) publish_cursor( cursor );

    SERVER_START_REQ( set_cursor )
    {
        req->flags = SET_CURSOR_HANDLE;
        req->handle = wine_server_user_handle( cursor );
        if ((ret = !wine_server_call_err( req )))
            old_cursor = wine_server_ptr_handle( reply->prev_handle );
    }
    SERVER_END_REQ;
    if (!ret) return 0;

    check_for_events( QS_INPUT );

    if (!(obj = get_icon_ptr( old_cursor ))) return 0;
    release_user_handle_ptr( obj );
    return old_cursor;
}

/***********************************************************************
 *	     NtUserGetCursor (win32u.@)
 */
HCURSOR WINAPI NtUserGetCursor(void)
{
    HCURSOR ret;

    SERVER_START_REQ( set_cursor )
    {
        req->flags = 0;
        wine_server_call( req );
        ret = wine_server_ptr_handle( reply->prev_handle );
    }
    SERVER_END_REQ;
    return ret;
}

HICON alloc_cursoricon_handle( BOOL is_icon )
{
    struct cursoricon_object *obj;
    HICON handle;

    if (!(obj = calloc( 1, sizeof(*obj) ))) return NULL;
    obj->is_icon = is_icon;
    if (!(handle = alloc_user_handle( obj, NTUSER_OBJ_ICON ))) free( obj );
    else obj->handle = handle;
    return handle;
}

static struct cursoricon_object *get_icon_frame_ptr( HICON handle, UINT step )
{
    struct cursoricon_object *obj, *ret;

    if (!(obj = get_icon_ptr( handle ))) return NULL;
    if (!obj->is_ani) return obj;
    if (step >= obj->ani.num_steps)
    {
        release_user_handle_ptr( obj );
        return NULL;
    }
    ret = get_icon_ptr( obj->ani.frames[step] );
    release_user_handle_ptr( obj );
    return ret;
}

static BOOL free_icon_handle( HICON handle )
{
    struct cursoricon_object *obj = free_user_handle( handle, NTUSER_OBJ_ICON );

    if (obj == OBJ_OTHER_PROCESS) WARN( "icon handle %p from other process\n", handle );
    else if (obj)
    {
        struct free_icon_params params = obj->params;
        void *ret_ptr;
        ULONG ret_len;
        UINT i;

        assert( !obj->rsrc );  /* shared icons can't be freed */

        if (!obj->is_ani)
        {
            if (obj->frame.alpha) NtGdiDeleteObjectApp( obj->frame.alpha );
            if (obj->frame.color) NtGdiDeleteObjectApp( obj->frame.color );
            if (obj->frame.mask)  NtGdiDeleteObjectApp( obj->frame.mask );
        }
        else
        {
            for (i = 0; i < obj->ani.num_steps; i++)
            {
                HICON hFrame = obj->ani.frames[i];

                if (hFrame)
                {
                    UINT j;

                    free_icon_handle( obj->ani.frames[i] );
                    for (j = 0; j < obj->ani.num_steps; j++)
                    {
                        if (obj->ani.frames[j] == hFrame) obj->ani.frames[j] = 0;
                    }
                }
            }
            free( obj->ani.frames );
        }
        if (!IS_INTRESOURCE( obj->resname )) free( obj->resname );
        if (obj->module.Length) free(obj->module.Buffer);
        if (obj->section) NtClose( obj->section );
        free( obj );
        KeUserDispatchCallback( &params.dispatch, sizeof(params), &ret_ptr, &ret_len );
        user_driver->pDestroyCursorIcon( handle );
        return TRUE;
    }
    return FALSE;
}

/***********************************************************************
 * Cross-process cursors
 *
 * Cursors are process-local objects in Wine, while on Windows they are
 * session-wide USER objects. The server routes WM_WINE_SETCURSOR to the
 * thread that owns the window under the pointer, and with out-of-process
 * child windows (WebView2 inside a host, plugins under a bridge) that is
 * not the process that created the cursor: get_icon_ptr() rejects the
 * handle and the driver keeps showing whatever it showed before.
 *
 * Instead of moving the bits into the server, the owning process publishes
 * the first frame in a named section when the cursor is first set
 * (publish_cursor), and a receiving process builds a process-local proxy
 * cursor from it (get_foreign_cursor) that the driver handles like any
 * other cursor. Proxies live in a small cache so the driver resources are
 * released again once a cursor falls out of use.
 */

#define SHARED_CURSOR_MAGIC       0x52554357  /* 'WCUR' */
#define FOREIGN_CURSOR_CACHE_SIZE 8
#define MAX_SHARED_CURSOR_SIZE    1024

struct shared_cursor_header
{
    UINT magic;
    UINT width;
    UINT height;        /* height of one frame */
    INT  xhot;
    INT  yhot;
    UINT color_size;    /* 32 bpp top-down bits following the header, 0 for monochrome */
    UINT mask_height;   /* rows in the mask bitmap, 2 * height for monochrome */
    UINT mask_size;     /* 1 bpp top-down bits following the color bits, DWORD-aligned rows */
};

static struct
{
    HCURSOR foreign;    /* handle owned by another process */
    HCURSOR local;      /* process-local proxy built from the published bits */
    UINT    last_use;
} foreign_cursors[FOREIGN_CURSOR_CACHE_SIZE];
static UINT foreign_cursor_clock;
/* > 0 while creating objects that must not be published: the frames of an animated
 * cursor (published through the outer object) and proxies (process-local by nature).
 * Always changed and read with user_lock held, so a plain counter is enough. */
static UINT publish_suppressed;

static void init_shared_cursor_name( HCURSOR handle, WCHAR *bufferW, UNICODE_STRING *name )
{
    char buffer[128];

    snprintf( buffer, sizeof(buffer), "\\Sessions\\%u\\BaseNamedObjects\\__wine_cursor_%08x",
              (unsigned int)NtCurrentTeb()->Peb->SessionId, wine_server_user_handle( handle ) );
    name->Buffer = bufferW;
    name->MaximumLength = asciiz_to_unicode( bufferW, buffer );
    name->Length = name->MaximumLength - sizeof(WCHAR);
}

static void init_dib_info( BITMAPINFO *info, UINT width, UINT height, UINT bpp )
{
    memset( &info->bmiHeader, 0, sizeof(info->bmiHeader) );
    info->bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    info->bmiHeader.biWidth       = width;
    info->bmiHeader.biHeight      = -(LONG)height;  /* top-down */
    info->bmiHeader.biPlanes      = 1;
    info->bmiHeader.biBitCount    = bpp;
    info->bmiHeader.biCompression = BI_RGB;
    info->bmiHeader.biSizeImage   = ((width * bpp + 31) / 32 * 4) * height;
    if (bpp == 1)
    {
        info->bmiHeader.biClrUsed = 2;
        memset( info->bmiColors, 0, 2 * sizeof(RGBQUAD) );
        info->bmiColors[1].rgbRed = info->bmiColors[1].rgbGreen = info->bmiColors[1].rgbBlue = 0xff;
    }
}

/* publish the first frame of a cursor owned by this process, so that a process
 * receiving WM_WINE_SETCURSOR for it can build a proxy */
static void publish_cursor( HCURSOR handle )
{
    char info_buf[FIELD_OFFSET( BITMAPINFO, bmiColors[256] )];
    BITMAPINFO *info = (BITMAPINFO *)info_buf;
    struct cursoricon_object *obj, *frame;
    struct shared_cursor_header hdr;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING name;
    WCHAR bufferW[128];
    LARGE_INTEGER size;
    SIZE_T view_size = 0;
    HANDLE section;
    void *view = NULL;
    char *bits;
    BOOL ok;
    BITMAP bm;
    HDC hdc;
    unsigned int status;

    obj = get_user_handle_ptr( handle, NTUSER_OBJ_ICON );
    if (!obj || obj == OBJ_OTHER_PROCESS) return;
    if (obj->section) goto done;  /* already published */
    if (!(frame = get_icon_frame_ptr( handle, 0 ))) goto done;
    if (!frame->frame.mask || !NtGdiExtGetObjectW( frame->frame.mask, sizeof(bm), &bm )) goto done_frame;

    hdr.magic       = SHARED_CURSOR_MAGIC;
    hdr.width       = frame->frame.width;
    hdr.height      = frame->frame.height;
    hdr.xhot        = frame->frame.hotspot.x;
    hdr.yhot        = frame->frame.hotspot.y;
    hdr.color_size  = frame->frame.color ? hdr.width * hdr.height * 4 : 0;
    hdr.mask_height = bm.bmHeight;
    hdr.mask_size   = ((hdr.width + 31) / 32 * 4) * hdr.mask_height;
    if (!hdr.width || !hdr.height || !hdr.mask_height ||
        hdr.width > MAX_SHARED_CURSOR_SIZE || hdr.height > MAX_SHARED_CURSOR_SIZE) goto done_frame;

    init_shared_cursor_name( handle, bufferW, &name );
    InitializeObjectAttributes( &attr, &name, 0, NULL, NULL );
    size.QuadPart = sizeof(hdr) + hdr.color_size + hdr.mask_size;
    if ((status = NtCreateSection( &section, SECTION_MAP_READ | SECTION_MAP_WRITE, &attr,
                                   &size, PAGE_READWRITE, SEC_COMMIT, 0 )))
    {
        WARN( "failed to create section for cursor %p, status %#x\n", handle, status );
        goto done_frame;
    }
    if ((status = NtMapViewOfSection( section, NtCurrentProcess(), &view, 0, 0, NULL, &view_size,
                                      ViewUnmap, 0, PAGE_READWRITE )))
    {
        WARN( "failed to map section for cursor %p, status %#x\n", handle, status );
        NtClose( section );
        goto done_frame;
    }

    hdc = NtGdiCreateCompatibleDC( 0 );
    memcpy( view, &hdr, sizeof(hdr) );
    bits = (char *)view + sizeof(hdr);
    ok = TRUE;
    if (hdr.color_size)
    {
        init_dib_info( info, hdr.width, hdr.height, 32 );
        ok = NtGdiGetDIBitsInternal( hdc, frame->frame.color, 0, hdr.height, bits, info,
                                     DIB_RGB_COLORS, hdr.color_size, sizeof(info_buf) ) == hdr.height;
        bits += hdr.color_size;
    }
    if (ok)
    {
        init_dib_info( info, hdr.width, hdr.mask_height, 1 );
        ok = NtGdiGetDIBitsInternal( hdc, frame->frame.mask, 0, hdr.mask_height, bits, info,
                                     DIB_RGB_COLORS, hdr.mask_size, sizeof(info_buf) ) == hdr.mask_height;
    }
    NtGdiDeleteObjectApp( hdc );
    NtUnmapViewOfSection( NtCurrentProcess(), view );
    if (!ok)
    {
        WARN( "failed to read bits of cursor %p\n", handle );
        NtClose( section );
        goto done_frame;
    }
    obj->section = section;
    TRACE( "cursor %p (%ux%u) published as %s\n", handle, hdr.width, hdr.height, debugstr_us(&name) );

done_frame:
    release_user_handle_ptr( frame );
done:
    release_user_handle_ptr( obj );
}

static HBITMAP create_dib_from_bits( HDC hdc, BITMAPINFO *info, const void *src, UINT size, void **bits_ret )
{
    void *bits = NULL;
    HBITMAP bitmap = NtGdiCreateDIBSection( hdc, NULL, 0, info, DIB_RGB_COLORS, 0, 0, 0, &bits );

    if (!bitmap) return 0;
    memcpy( bits, src, size );
    if (bits_ret) *bits_ret = bits;
    return bitmap;
}

/* build a process-local cursor from the bits published by the owning process */
static HCURSOR create_proxy_cursor( HCURSOR foreign )
{
    char info_buf[FIELD_OFFSET( BITMAPINFO, bmiColors[256] )];
    BITMAPINFO *info = (BITMAPINFO *)info_buf;
    struct cursoricon_frame frame = {0};
    struct cursoricon_desc desc = { .frames = &frame };
    const struct shared_cursor_header *hdr;
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING name;
    WCHAR bufferW[128];
    SIZE_T view_size = 0;
    HANDLE section;
    void *view = NULL;
    HCURSOR local = 0;
    HDC hdc = 0;
    unsigned int status;
    BOOL ok;

    init_shared_cursor_name( foreign, bufferW, &name );
    InitializeObjectAttributes( &attr, &name, 0, NULL, NULL );
    if ((status = NtOpenSection( &section, SECTION_MAP_READ, &attr )))
    {
        WARN( "cursor %p from other process has no published bits, status %#x\n", foreign, status );
        return 0;
    }
    if ((status = NtMapViewOfSection( section, NtCurrentProcess(), &view, 0, 0, NULL, &view_size,
                                      ViewUnmap, 0, PAGE_READONLY )))
    {
        WARN( "failed to map published bits of cursor %p, status %#x\n", foreign, status );
        NtClose( section );
        return 0;
    }

    hdr = view;
    if (view_size < sizeof(*hdr) || hdr->magic != SHARED_CURSOR_MAGIC ||
        !hdr->width || !hdr->height ||
        hdr->width > MAX_SHARED_CURSOR_SIZE || hdr->height > MAX_SHARED_CURSOR_SIZE ||
        (hdr->color_size && hdr->color_size != hdr->width * hdr->height * 4) ||
        (hdr->mask_height != hdr->height && hdr->mask_height != 2 * hdr->height) ||
        hdr->mask_size != ((hdr->width + 31) / 32 * 4) * hdr->mask_height ||
        (ULONGLONG)sizeof(*hdr) + hdr->color_size + hdr->mask_size > view_size)
    {
        WARN( "invalid published bits for cursor %p\n", foreign );
        goto done;
    }

    hdc = NtGdiCreateCompatibleDC( 0 );
    frame.width     = hdr->width;
    frame.height    = hdr->height;
    frame.hotspot.x = hdr->xhot;
    frame.hotspot.y = hdr->yhot;
    if (hdr->color_size)
    {
        const DWORD *src = (const DWORD *)(hdr + 1);
        UINT i, count = hdr->width * hdr->height;
        unsigned char *ptr;

        init_dib_info( info, hdr->width, hdr->height, 32 );
        if (!(frame.color = create_dib_from_bits( hdc, info, src, hdr->color_size, NULL ))) goto done;
        for (i = 0; i < count; i++) if (src[i] & 0xff000000) break;
        if (i < count)  /* has an alpha channel: build the pre-multiplied copy that DrawIconEx uses */
        {
            if (!(frame.alpha = create_dib_from_bits( hdc, info, src, hdr->color_size, (void **)&ptr ))) goto done;
            for (i = 0; i < count; i++, ptr += 4)
            {
                unsigned int alpha = ptr[3];
                ptr[0] = (ptr[0] * alpha + 127) / 255;
                ptr[1] = (ptr[1] * alpha + 127) / 255;
                ptr[2] = (ptr[2] * alpha + 127) / 255;
            }
        }
    }
    init_dib_info( info, hdr->width, hdr->mask_height, 1 );
    if (!(frame.mask = create_dib_from_bits( hdc, info, (const char *)(hdr + 1) + hdr->color_size,
                                             hdr->mask_size, NULL ))) goto done;

    if (!(local = alloc_cursoricon_handle( FALSE ))) goto done;
    user_lock();
    publish_suppressed++;
    ok = NtUserSetCursorIconData( local, NULL, NULL, &desc );
    publish_suppressed--;
    user_unlock();
    if (!ok)
    {
        free_icon_handle( local );
        local = 0;
        goto done;
    }
    frame.color = frame.alpha = frame.mask = 0;  /* owned by the proxy object now */
    TRACE( "cursor %p from other process -> proxy %p (%ux%u)\n", foreign, local, hdr->width, hdr->height );

done:
    if (frame.color) NtGdiDeleteObjectApp( frame.color );
    if (frame.alpha) NtGdiDeleteObjectApp( frame.alpha );
    if (frame.mask)  NtGdiDeleteObjectApp( frame.mask );
    if (hdc) NtGdiDeleteObjectApp( hdc );
    NtUnmapViewOfSection( NtCurrentProcess(), view );
    NtClose( section );
    return local;
}

/* user_lock must be held by caller */
static HCURSOR find_foreign_cursor( HCURSOR foreign )
{
    unsigned int i;

    for (i = 0; i < FOREIGN_CURSOR_CACHE_SIZE; i++)
    {
        if (foreign_cursors[i].local && foreign_cursors[i].foreign == foreign)
        {
            foreign_cursors[i].last_use = ++foreign_cursor_clock;
            return foreign_cursors[i].local;
        }
    }
    return 0;
}

/* map a cursor handle owned by another process to a local proxy the driver can use */
static HCURSOR get_foreign_cursor( HCURSOR foreign )
{
    HCURSOR local, proxy, evicted = 0;
    unsigned int i, slot = 0;

    user_lock();
    local = find_foreign_cursor( foreign );
    user_unlock();
    if (local) return local;

    if (!(proxy = create_proxy_cursor( foreign ))) return 0;

    user_lock();
    if ((local = find_foreign_cursor( foreign ))) evicted = proxy;  /* another thread was faster */
    else
    {
        for (i = 0; i < FOREIGN_CURSOR_CACHE_SIZE; i++)
        {
            if (!foreign_cursors[i].local) { slot = i; break; }
            if (foreign_cursors[i].last_use < foreign_cursors[slot].last_use) slot = i;
        }
        evicted = foreign_cursors[slot].local;
        foreign_cursors[slot].foreign  = foreign;
        foreign_cursors[slot].local    = proxy;
        foreign_cursors[slot].last_use = ++foreign_cursor_clock;
        local = proxy;
    }
    user_unlock();

    if (evicted) free_icon_handle( evicted );
    return local;
}

/***********************************************************************
 *	     NtUserDestroyCursor (win32u.@)
 */
BOOL WINAPI NtUserDestroyCursor( HCURSOR cursor, ULONG arg )
{
    struct cursoricon_object *obj;
    BOOL shared, ret;

    TRACE( "%p\n", cursor );

    if (!(obj = get_icon_ptr( cursor ))) return FALSE;
    shared = obj->is_shared;
    release_user_handle_ptr( obj );
    ret = NtUserGetCursor() != cursor;
    if (!shared) free_icon_handle( cursor );
    return ret;
}

/***********************************************************************
 *	     NtUserSetCursorIconData (win32u.@)
 */
BOOL WINAPI NtUserSetCursorIconData( HCURSOR cursor, UNICODE_STRING *module, UNICODE_STRING *res_name,
                                     struct cursoricon_desc *desc )
{
    struct cursoricon_object *obj;
    UINT i, j;

    if (!(obj = get_icon_ptr( cursor ))) return FALSE;

    if (obj->is_ani || obj->frame.width)
    {
        /* already initialized */
        release_user_handle_ptr( obj );
        RtlSetLastWin32Error( ERROR_INVALID_CURSOR_HANDLE );
        return FALSE;
    }

    obj->delay = desc->delay;

    if (desc->num_steps)
    {
        if (!(obj->ani.frames = calloc( desc->num_steps, sizeof(*obj->ani.frames) )))
        {
            release_user_handle_ptr( obj );
            return FALSE;
        }
        obj->is_ani = TRUE;
        obj->ani.num_steps  = desc->num_steps;
        obj->ani.num_frames = desc->num_frames;
    }
    else obj->frame = desc->frames[0];

    if (!res_name)
        obj->resname = NULL;
    else if (res_name->Length)
    {
        obj->resname = malloc( res_name->Length + sizeof(WCHAR) );
        if (obj->resname)
        {
            memcpy( obj->resname, res_name->Buffer, res_name->Length );
            obj->resname[res_name->Length / sizeof(WCHAR)] = 0;
        }
    }
    else
        obj->resname = MAKEINTRESOURCEW( LOWORD(res_name->Buffer) );

    if (module && module->Length && (obj->module.Buffer = malloc( module->Length )))
    {
        memcpy( obj->module.Buffer, module->Buffer, module->Length );
        obj->module.Length = module->Length;
    }

    if (obj->is_ani)
    {
        /* Setup the animated frames in the correct sequence */
        for (i = 0; i < desc->num_steps; i++)
        {
            struct cursoricon_desc frame_desc;
            DWORD frame_id;
            BOOL ok;

            if (obj->ani.frames[i]) continue; /* already set */

            frame_id = desc->frame_seq ? desc->frame_seq[i] : i;
            if (frame_id >= obj->ani.num_frames)
            {
                frame_id = obj->ani.num_frames - 1;
                ERR_(cursor)( "Sequence indicates frame past end of list, corrupt?\n" );
            }
            memset( &frame_desc, 0, sizeof(frame_desc) );
            frame_desc.delay  = desc->frame_rates ? desc->frame_rates[i] : desc->delay;
            frame_desc.frames = &desc->frames[frame_id];
            if (!(obj->ani.frames[i] = alloc_cursoricon_handle( obj->is_icon )))
            {
                release_user_handle_ptr( obj );
                return 0;
            }
            publish_suppressed++;  /* frames are published through the outer object only */
            ok = NtUserSetCursorIconData( obj->ani.frames[i], NULL, NULL, &frame_desc );
            publish_suppressed--;
            if (!ok)
            {
                release_user_handle_ptr( obj );
                return 0;
            }

            if (desc->frame_seq)
            {
                for (j = i + 1; j < obj->ani.num_steps; j++)
                {
                    if (desc->frame_seq[j] == frame_id) obj->ani.frames[j] = obj->ani.frames[i];
                }
            }
        }
    }

    if (desc->flags & LR_SHARED)
    {
        obj->is_shared = TRUE;
        if (obj->module.Length)
        {
            obj->rsrc = desc->rsrc;
            list_add_head( &icon_cache, &obj->entry );
        }
    }

    /* make cursors reachable from other processes right away: any process may
     * pass this handle to SetCursor(), not only the one that created it */
    if (!obj->is_icon && !publish_suppressed) publish_cursor( cursor );

    release_user_handle_ptr( obj );
    return TRUE;
}

/***********************************************************************
 *	     NtUserFindExistingCursorIcon (win32u.@)
 */
HICON WINAPI NtUserFindExistingCursorIcon( UNICODE_STRING *module, UNICODE_STRING *res_name, void *desc )
{
    struct cursoricon_object *ptr;
    HICON ret = 0;

    user_lock();
    LIST_FOR_EACH_ENTRY( ptr, &icon_cache, struct cursoricon_object, entry )
    {
        if (ptr->module.Length != module->Length) continue;
        if (memcmp( ptr->module.Buffer, module->Buffer, module->Length )) continue;
        /* We pass rsrc as desc argument, this is not compatible with Windows */
        if (ptr->rsrc != desc) continue;
        ret = ptr->handle;
        break;
    }
    user_unlock();
    return ret;
}

/***********************************************************************
 *	     NtUserGetIconSize (win32u.@)
 */
BOOL WINAPI NtUserGetIconSize( HICON handle, UINT step, LONG *width, LONG *height )
{
    struct cursoricon_object *obj;

    if (!(obj = get_icon_frame_ptr( handle, step )))
    {
        RtlSetLastWin32Error( ERROR_INVALID_CURSOR_HANDLE );
        return FALSE;
    }

    *width  = obj->frame.width;
    *height = obj->frame.height * 2;
    release_user_handle_ptr( obj );
    return TRUE;
}

/**********************************************************************
 *           NtUserGetCursorFrameInfo (win32u.@)
 */
HCURSOR WINAPI NtUserGetCursorFrameInfo( HCURSOR cursor, DWORD istep, DWORD *rate_jiffies,
                                         DWORD *num_steps )
{
    struct cursoricon_object *obj;
    HCURSOR ret = 0;
    UINT icon_steps;

    if (!rate_jiffies || !num_steps) return 0;

    if (!(obj = get_icon_ptr( cursor ))) return 0;

    TRACE( "%p => %d %p %p\n", cursor, istep, rate_jiffies, num_steps );

    icon_steps = obj->is_ani ? obj->ani.num_steps : 1;
    if (istep < icon_steps || !obj->is_ani)
    {
        UINT icon_frames = 1;

        if (obj->is_ani)
            icon_frames = obj->ani.num_frames;
        if (obj->is_ani && icon_frames > 1)
            ret = obj->ani.frames[istep];
        else
            ret = cursor;
        if (icon_frames == 1)
        {
            *rate_jiffies = 0;
            *num_steps = 1;
        }
        else if (icon_steps == 1)
        {
            *num_steps = ~0;
            *rate_jiffies = obj->delay;
        }
        else if (istep < icon_steps)
        {
            struct cursoricon_object *frame;

            *num_steps = icon_steps;
            frame = get_icon_ptr( obj->ani.frames[istep] );
            if (obj->ani.num_steps == 1)
                *num_steps = ~0;
            else
                *num_steps = obj->ani.num_steps;
            *rate_jiffies = frame->delay;
            release_user_handle_ptr( frame );
        }
    }

    release_user_handle_ptr( obj );
    return ret;
}

/***********************************************************************
 *             copy_bitmap
 *
 * Helper function to duplicate a bitmap.
 */
static HBITMAP copy_bitmap( HBITMAP bitmap, const SIZE *size )
{
    HDC src, dst = 0;
    HBITMAP new_bitmap = 0;
    BITMAP bmp;

    if (!bitmap) return 0;
    if (!NtGdiExtGetObjectW( bitmap, sizeof(bmp), &bmp )) return 0;

    if ((src = NtGdiCreateCompatibleDC( 0 )) && (dst = NtGdiCreateCompatibleDC( 0 )))
    {
        NtGdiSelectBitmap( src, bitmap );
        if ((new_bitmap = NtGdiCreateCompatibleBitmap( src, size ? size->cx : bmp.bmWidth, size ? size->cy : bmp.bmHeight )))
        {
            NtGdiSelectBitmap( dst, new_bitmap );
            if (size) NtGdiStretchBlt( dst, 0, 0, size->cx, size->cy, src, 0, 0, bmp.bmWidth, bmp.bmHeight, SRCCOPY, 0 );
            else NtGdiBitBlt( dst, 0, 0, bmp.bmWidth, bmp.bmHeight, src, 0, 0, SRCCOPY, 0, 0 );
        }
    }
    NtGdiDeleteObjectApp( dst );
    NtGdiDeleteObjectApp( src );
    return new_bitmap;
}

/**********************************************************************
 *           NtUserGetIconInfo (win32u.@)
 */
BOOL WINAPI NtUserGetIconInfo( HICON icon, ICONINFO *info, UNICODE_STRING *module,
                               UNICODE_STRING *res_name, DWORD *bpp, LONG unk )
{
    struct cursoricon_object *obj, *frame_obj;
    BOOL ret = TRUE;

    if (!(obj = get_icon_ptr( icon )))
    {
        RtlSetLastWin32Error( ERROR_INVALID_CURSOR_HANDLE );
        return FALSE;
    }
    if (!(frame_obj = get_icon_frame_ptr( icon, 0 )))
    {
        release_user_handle_ptr( obj );
        return FALSE;
    }

    TRACE( "%p => %dx%d\n", icon, frame_obj->frame.width, frame_obj->frame.height );

    info->fIcon        = obj->is_icon;
    info->xHotspot     = frame_obj->frame.hotspot.x;
    info->yHotspot     = frame_obj->frame.hotspot.y;
    info->hbmColor     = copy_bitmap( frame_obj->frame.color, NULL );
    info->hbmMask      = copy_bitmap( frame_obj->frame.mask, NULL );
    if (!info->hbmMask || (!info->hbmColor && frame_obj->frame.color))
    {
        NtGdiDeleteObjectApp( info->hbmMask );
        NtGdiDeleteObjectApp( info->hbmColor );
        ret = FALSE;
    }
    else if (obj->module.Length)
    {
        if (module)
        {
            size_t size = min( module->MaximumLength, obj->module.Length );
            if (size) memcpy( module->Buffer, obj->module.Buffer, size );
            module->Length = size / sizeof(WCHAR); /* length in chars, not bytes */
        }
        if (res_name)
        {
            if (IS_INTRESOURCE( obj->resname ))
            {
                res_name->Buffer = obj->resname;
                res_name->Length = 0;
            }
            else
            {
                size_t size = min( res_name->MaximumLength, lstrlenW( obj->resname) * sizeof(WCHAR) );
                if (size) memcpy( res_name->Buffer, obj->resname, size );
                res_name->Length = size / sizeof(WCHAR); /* length in chars, not bytes */
            }
        }
    }
    else
    {
        if (module) module->Length = 0;
        if (res_name)
        {
            res_name->Length = 0;
            res_name->Buffer = NULL;
        }
    }
    release_user_handle_ptr( frame_obj );
    release_user_handle_ptr( obj );
    return ret;
}

/******************************************************************************
 *	     NtUserDrawIconEx (win32u.@)
 */
BOOL WINAPI NtUserDrawIconEx( HDC hdc, INT x0, INT y0, HICON icon, INT width,
                              INT height, UINT step, HBRUSH brush, UINT flags )
{
    struct cursoricon_object *obj;
    HBITMAP offscreen_bitmap = 0;
    HDC hdc_dest, mem_dc;
    COLORREF old_fg, old_bg;
    INT x, y, nStretchMode;
    BOOL result = FALSE;

    TRACE_(icon)( "(hdc=%p,pos=%d.%d,hicon=%p,extend=%d.%d,step=%d,br=%p,flags=0x%08x)\n",
                  hdc, x0, y0, icon, width, height, step, brush, flags );

    if (!(obj = get_icon_frame_ptr( icon, step )))
    {
        FIXME_(icon)("Error retrieving icon frame %d\n", step);
        return FALSE;
    }
    if (!(mem_dc = NtGdiCreateCompatibleDC( hdc )))
    {
        release_user_handle_ptr( obj );
        return FALSE;
    }

    if (flags & DI_NOMIRROR)
        FIXME_(icon)("Ignoring flag DI_NOMIRROR\n");

    /* Calculate the size of the destination image.  */
    if (width == 0)
    {
        if (flags & DI_DEFAULTSIZE)
            width = get_system_metrics( SM_CXICON );
        else
            width = obj->frame.width;
    }
    if (height == 0)
    {
        if (flags & DI_DEFAULTSIZE)
            height = get_system_metrics( SM_CYICON );
        else
            height = obj->frame.height;
    }

    if (get_gdi_object_type( brush ) == NTGDI_OBJ_BRUSH)
    {
        HBRUSH prev_brush;
        RECT r;

        SetRect(&r, 0, 0, width, width);

        if (!(hdc_dest = NtGdiCreateCompatibleDC(hdc))) goto failed;
        if (!(offscreen_bitmap = NtGdiCreateCompatibleBitmap(hdc, width, height)))
        {
            NtGdiDeleteObjectApp( hdc_dest );
            goto failed;
        }
        NtGdiSelectBitmap( hdc_dest, offscreen_bitmap );

        prev_brush = NtGdiSelectBrush( hdc_dest, brush );
        NtGdiPatBlt( hdc_dest, r.left, r.top, r.right - r.left, r.bottom - r.top, PATCOPY );
        if (prev_brush) NtGdiSelectBrush( hdc_dest, prev_brush );
        x = y = 0;
    }
    else
    {
        hdc_dest = hdc;
        x = x0;
        y = y0;
    }

    nStretchMode = set_stretch_blt_mode( hdc, STRETCH_DELETESCANS );
    NtGdiGetAndSetDCDword( hdc, NtGdiSetTextColor, RGB(0,0,0), &old_fg );
    NtGdiGetAndSetDCDword( hdc, NtGdiSetBkColor, RGB(255,255,255), &old_bg );

    if (obj->frame.alpha && (flags & DI_IMAGE))
    {
        BOOL alpha_blend = TRUE;

        if (get_gdi_object_type( hdc_dest ) == NTGDI_OBJ_MEMDC)
        {
            BITMAP bm;
            HBITMAP bmp = NtGdiGetDCObject( hdc_dest, NTGDI_OBJ_SURF );
            alpha_blend = NtGdiExtGetObjectW( bmp, sizeof(bm), &bm ) && bm.bmBitsPixel > 8;
        }
        if (alpha_blend)
        {
            NtGdiSelectBitmap( mem_dc, obj->frame.alpha );
            if (NtGdiAlphaBlend( hdc_dest, x, y, width, height, mem_dc,
                                 0, 0, obj->frame.width, obj->frame.height,
                                 MAKEFOURCC( AC_SRC_OVER, 0, 255, AC_SRC_ALPHA ), 0 ))
                goto done;
        }
    }

    if (flags & DI_MASK)
    {
        DWORD rop = (flags & DI_IMAGE) ? SRCAND : SRCCOPY;
        NtGdiSelectBitmap( mem_dc, obj->frame.mask );
        NtGdiStretchBlt( hdc_dest, x, y, width, height,
                         mem_dc, 0, 0, obj->frame.width, obj->frame.height, rop, 0 );
    }

    if (flags & DI_IMAGE)
    {
        if (obj->frame.color)
        {
            DWORD rop = (flags & DI_MASK) ? SRCINVERT : SRCCOPY;
            NtGdiSelectBitmap( mem_dc, obj->frame.color );
            NtGdiStretchBlt( hdc_dest, x, y, width, height,
                             mem_dc, 0, 0, obj->frame.width, obj->frame.height, rop, 0 );
        }
        else
        {
            DWORD rop = (flags & DI_MASK) ? SRCINVERT : SRCCOPY;
            NtGdiSelectBitmap( mem_dc, obj->frame.mask );
            NtGdiStretchBlt( hdc_dest, x, y, width, height,
                             mem_dc, 0, obj->frame.height, obj->frame.width,
                             obj->frame.height, rop, 0 );
        }
    }

done:
    if (offscreen_bitmap) NtGdiBitBlt( hdc, x0, y0, width, height, hdc_dest, 0, 0, SRCCOPY, 0, 0 );

    NtGdiGetAndSetDCDword( hdc, NtGdiSetTextColor, old_fg, NULL );
    NtGdiGetAndSetDCDword( hdc, NtGdiSetBkColor, old_bg, NULL );
    nStretchMode = set_stretch_blt_mode( hdc, nStretchMode );

    result = TRUE;
    if (hdc_dest != hdc) NtGdiDeleteObjectApp( hdc_dest );
    if (offscreen_bitmap) NtGdiDeleteObjectApp( offscreen_bitmap );
failed:
    NtGdiDeleteObjectApp( mem_dc );
    release_user_handle_ptr( obj );
    return result;
}

ULONG_PTR get_icon_param( HICON handle )
{
    ULONG_PTR ret = 0;
    struct cursoricon_object *obj = get_user_handle_ptr( handle, NTUSER_OBJ_ICON );

    if (obj == OBJ_OTHER_PROCESS) WARN( "icon handle %p from other process\n", handle );
    else if (obj)
    {
        ret = obj->params.param;
        release_user_handle_ptr( obj );
    }
    return ret;
}

ULONG_PTR set_icon_param( HICON handle, const struct free_icon_params *params )
{
    ULONG_PTR ret = 0;
    struct cursoricon_object *obj = get_user_handle_ptr( handle, NTUSER_OBJ_ICON );

    if (obj == OBJ_OTHER_PROCESS) WARN( "icon handle %p from other process\n", handle );
    else if (obj)
    {
        ret = obj->params.param;
        obj->params = *params;
        release_user_handle_ptr( obj );
    }
    return ret;
}

HICON create_small_icon( HICON handle )
{
    struct cursoricon_frame frame = {0};
    struct cursoricon_desc desc = {0};
    struct cursoricon_object *obj;
    HICON icon;
    SIZE size;

    if (!handle) return 0;

    desc.frames = &frame;
    frame.width = size.cx = get_system_metrics( SM_CXSMICON );
    frame.height = size.cy = get_system_metrics( SM_CYSMICON );
    frame.hotspot.x = frame.width / 2;
    frame.hotspot.y = frame.height / 2;

    if (!(obj = get_user_handle_ptr( handle, NTUSER_OBJ_ICON )) || obj == OBJ_OTHER_PROCESS) return 0;
    frame.color = copy_bitmap( obj->frame.color, &size );
    frame.alpha = copy_bitmap( obj->frame.alpha, &size );
    frame.mask = copy_bitmap( obj->frame.mask, &size );
    release_user_handle_ptr( obj );

    if (!(icon = alloc_cursoricon_handle( TRUE )) || !NtUserSetCursorIconData( icon, NULL, NULL, &desc ))
    {
        NtGdiDeleteObjectApp( frame.color );
        NtGdiDeleteObjectApp( frame.alpha );
        NtGdiDeleteObjectApp( frame.mask );
        NtUserDestroyCursor( icon, 0 );
        return 0;
    }

    return icon;
}

/******************************************************************************
 *	     CopyImage (win32u.so)
 */
HANDLE WINAPI CopyImage( HANDLE hwnd, UINT type, INT dx, INT dy, UINT flags )
{
    void *ret_ptr;
    ULONG ret_len;
    NTSTATUS ret;
    struct copy_image_params params =
        { .hwnd = hwnd, .type = type, .dx = dx, .dy = dy, .flags = flags };

    ret = KeUserModeCallback( NtUserCopyImage, &params, sizeof(params), &ret_ptr, &ret_len );
    if (!ret && ret_len == sizeof(HANDLE)) return *(HANDLE *)ret_ptr;
    return 0;
}

/******************************************************************************
 *           LoadImage (win32u.so)
 */
HANDLE WINAPI LoadImageW( HINSTANCE hinst, const WCHAR *name, UINT type,
                          INT dx, INT dy, UINT flags )
{
    void *ret_ptr;
    ULONG ret_len;
    NTSTATUS ret;
    struct load_image_params params =
        { .hinst = hinst, .name = name, .type = type, .dx = dx, .dy = dy, .flags = flags };

    if (HIWORD(name))
    {
        ERR( "name %s not supported in Unix modules\n", debugstr_w( name ));
        return 0;
    }
    ret = KeUserModeCallback( NtUserLoadImage, &params, sizeof(params), &ret_ptr, &ret_len );
    if (!ret && ret_len == sizeof(HANDLE)) return *(HANDLE *)ret_ptr;
    return 0;
}
