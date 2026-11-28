/*
 * The canvas element, with the part of its 2D context that measures text.
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

#include <stdarg.h>
#include <math.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "wingdi.h"
#include "ole2.h"

#include "wine/debug.h"

#include "mshtml_private.h"
#include "htmlevent.h"

WINE_DEFAULT_DEBUG_CHANNEL(mshtml);

/* The font of a 2D context, as parsed from the CSS font shorthand. */
struct canvas_font
{
    float size;                 /* in CSS pixels */
    LONG weight;
    BOOL italic;
    WCHAR face[LF_FACESIZE];
};

struct text_metrics
{
    DispatchEx dispex;
    IWineMSHTMLTextMetrics IWineMSHTMLTextMetrics_iface;
    float width;
};

struct canvas_context
{
    DispatchEx dispex;
    IWineMSHTMLCanvasRenderingContext2D IWineMSHTMLCanvasRenderingContext2D_iface;
    WCHAR *font_str;
    struct canvas_font font;
};

typedef struct
{
    HTMLElement element;
    IWineHTMLCanvasPrivate IWineHTMLCanvasPrivate_iface;
    struct canvas_context *context;
} HTMLCanvasElement;

/* TextMetrics */

static inline struct text_metrics *impl_from_IWineMSHTMLTextMetrics(IWineMSHTMLTextMetrics *iface)
{
    return CONTAINING_RECORD(iface, struct text_metrics, IWineMSHTMLTextMetrics_iface);
}

DISPEX_IDISPATCH_IMPL(text_metrics, IWineMSHTMLTextMetrics,
                      impl_from_IWineMSHTMLTextMetrics(iface)->dispex)

static HRESULT WINAPI text_metrics_get_width(IWineMSHTMLTextMetrics *iface, float *p)
{
    struct text_metrics *metrics = impl_from_IWineMSHTMLTextMetrics(iface);

    TRACE("(%p)->(%p)\n", metrics, p);

    *p = metrics->width;
    return S_OK;
}

static const IWineMSHTMLTextMetricsVtbl text_metrics_vtbl = {
    text_metrics_QueryInterface,
    text_metrics_AddRef,
    text_metrics_Release,
    text_metrics_GetTypeInfoCount,
    text_metrics_GetTypeInfo,
    text_metrics_GetIDsOfNames,
    text_metrics_Invoke,
    text_metrics_get_width
};

static inline struct text_metrics *text_metrics_from_DispatchEx(DispatchEx *iface)
{
    return CONTAINING_RECORD(iface, struct text_metrics, dispex);
}

static void *TextMetrics_query_interface(DispatchEx *dispex, REFIID riid)
{
    struct text_metrics *metrics = text_metrics_from_DispatchEx(dispex);

    if(IsEqualGUID(&IID_IWineMSHTMLTextMetrics, riid))
        return &metrics->IWineMSHTMLTextMetrics_iface;

    return NULL;
}

static void TextMetrics_destructor(DispatchEx *dispex)
{
    struct text_metrics *metrics = text_metrics_from_DispatchEx(dispex);
    free(metrics);
}

static const dispex_static_data_vtbl_t TextMetrics_dispex_vtbl = {
    .query_interface  = TextMetrics_query_interface,
    .destructor       = TextMetrics_destructor,
};

static const tid_t TextMetrics_iface_tids[] = {
    IWineMSHTMLTextMetrics_tid,
    0
};
dispex_static_data_t TextMetrics_dispex = {
    .id              = OBJID_TextMetrics,
    .vtbl            = &TextMetrics_dispex_vtbl,
    .disp_tid        = IWineMSHTMLTextMetrics_tid,
    .iface_tids      = TextMetrics_iface_tids,
    .min_compat_mode = COMPAT_MODE_IE9,
};

/* The font shorthand */

static const WCHAR *skip_space(const WCHAR *p)
{
    while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f')
        p++;
    return p;
}

static BOOL is_token_end(WCHAR c)
{
    return !c || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static BOOL token_equals(const WCHAR *token, size_t len, const WCHAR *keyword)
{
    return wcslen(keyword) == len && !wcsnicmp(token, keyword, len);
}

static int CALLBACK font_exists_proc(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lparam)
{
    *(BOOL*)lparam = TRUE;
    return 0;
}

static BOOL font_exists(HDC hdc, const WCHAR *face)
{
    BOOL ret = FALSE;
    LOGFONTW lf;

    memset(&lf, 0, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, face, ARRAY_SIZE(lf.lfFaceName));
    EnumFontFamiliesExW(hdc, &lf, font_exists_proc, (LPARAM)&ret, 0);
    return ret;
}

static const WCHAR *map_generic_family(const WCHAR *name)
{
    static const struct {
        const WCHAR *generic;
        const WCHAR *face;
    } generic_families[] = {
        {L"sans-serif", L"Arial"},
        {L"serif",      L"Times New Roman"},
        {L"monospace",  L"Courier New"},
        {L"cursive",    L"Comic Sans MS"},
        {L"fantasy",    L"Impact"},
    };
    unsigned i;

    for(i = 0; i < ARRAY_SIZE(generic_families); i++) {
        if(!wcsicmp(name, generic_families[i].generic))
            return generic_families[i].face;
    }
    return name;
}

/* Picks the first installed family of a comma separated list. FALSE if the list is malformed. */
static BOOL parse_font_families(const WCHAR *p, WCHAR *face)
{
    BOOL found = FALSE, any = FALSE;
    WCHAR name[LF_FACESIZE];
    HDC hdc;

    hdc = CreateCompatibleDC(NULL);

    for(;;) {
        const WCHAR *start, *end;
        size_t len;

        p = skip_space(p);
        if(*p == '"' || *p == '\'') {
            WCHAR quote = *p++;
            start = p;
            while(*p && *p != quote)
                p++;
            if(!*p)
                goto invalid;
            end = p++;
            p = skip_space(p);
            if(*p && *p != ',')
                goto invalid;
        }else {
            start = p;
            while(*p && *p != ',')
                p++;
            end = p;
            while(end > start && is_token_end(end[-1]))
                end--;
        }

        len = end - start;
        if(!len)
            goto invalid;
        any = TRUE;

        if(!found && len < ARRAY_SIZE(name)) {
            const WCHAR *mapped;

            memcpy(name, start, len * sizeof(WCHAR));
            name[len] = 0;
            mapped = map_generic_family(name);
            if(hdc && font_exists(hdc, mapped)) {
                lstrcpynW(face, mapped, LF_FACESIZE);
                found = TRUE;
            }
        }

        if(!*p)
            break;
        p++; /* the comma */
    }

    if(hdc)
        DeleteDC(hdc);
    if(!any)
        return FALSE;
    if(!found)
        lstrcpyW(face, L"Arial");
    return TRUE;

invalid:
    if(hdc)
        DeleteDC(hdc);
    return FALSE;
}

/* [ style || variant || weight ]? size[/line-height] family[, family]* */
static BOOL parse_canvas_font(const WCHAR *str, struct canvas_font *font)
{
    struct canvas_font ret = { .weight = FW_NORMAL };
    const WCHAR *p = skip_space(str), *end;
    double size;
    WCHAR *num_end;

    for(;;) {
        size_t len;

        for(end = p; !is_token_end(*end); end++);
        len = end - p;
        if(!len)
            return FALSE;

        if(token_equals(p, len, L"normal") || token_equals(p, len, L"small-caps")) {
            /* nothing to do */
        }else if(token_equals(p, len, L"italic") || token_equals(p, len, L"oblique")) {
            ret.italic = TRUE;
        }else if(token_equals(p, len, L"bold") || token_equals(p, len, L"bolder")) {
            ret.weight = FW_BOLD;
        }else if(token_equals(p, len, L"lighter")) {
            ret.weight = FW_LIGHT;
        }else if(len == 3 && p[0] >= '1' && p[0] <= '9' && p[1] == '0' && p[2] == '0') {
            ret.weight = (p[0] - '0') * 100;
        }else {
            break;
        }
        p = skip_space(end);
    }

    if(!(*p >= '0' && *p <= '9') && *p != '.')
        return FALSE;
    size = wcstod(p, &num_end);
    if(num_end == p || !(size >= 0))
        return FALSE;
    p = num_end;

    if(!wcsnicmp(p, L"px", 2)) {
        /* CSS pixels */
    }else if(!wcsnicmp(p, L"pt", 2)) {
        size = size * 96.0 / 72.0;
    }else {
        return FALSE;
    }
    p += 2;
    ret.size = size;

    if(*p == '/' || *skip_space(p) == '/') {
        /* The line height does not change the width of a text. */
        p = skip_space(p);
        p = skip_space(p + 1);
        while(!is_token_end(*p))
            p++;
    }else if(!is_token_end(*p)) {
        return FALSE;
    }

    if(!parse_font_families(p, ret.face))
        return FALSE;

    *font = ret;
    return TRUE;
}

/* CanvasRenderingContext2D */

static inline struct canvas_context *impl_from_IWineMSHTMLCanvasRenderingContext2D(IWineMSHTMLCanvasRenderingContext2D *iface)
{
    return CONTAINING_RECORD(iface, struct canvas_context, IWineMSHTMLCanvasRenderingContext2D_iface);
}

DISPEX_IDISPATCH_IMPL(canvas_context, IWineMSHTMLCanvasRenderingContext2D,
                      impl_from_IWineMSHTMLCanvasRenderingContext2D(iface)->dispex)

static HRESULT WINAPI canvas_context_put_font(IWineMSHTMLCanvasRenderingContext2D *iface, BSTR v)
{
    struct canvas_context *context = impl_from_IWineMSHTMLCanvasRenderingContext2D(iface);
    struct canvas_font font;
    WCHAR *str;

    TRACE("(%p)->(%s)\n", context, debugstr_w(v));

    /* A value that is not a font shorthand leaves the font as it is. */
    if(!v || !parse_canvas_font(v, &font)) {
        WARN("Ignoring font %s\n", debugstr_w(v));
        return S_OK;
    }

    if(!(str = wcsdup(v)))
        return E_OUTOFMEMORY;

    free(context->font_str);
    context->font_str = str;
    context->font = font;
    return S_OK;
}

static HRESULT WINAPI canvas_context_get_font(IWineMSHTMLCanvasRenderingContext2D *iface, BSTR *p)
{
    struct canvas_context *context = impl_from_IWineMSHTMLCanvasRenderingContext2D(iface);

    TRACE("(%p)->(%p)\n", context, p);

    if(!(*p = SysAllocString(context->font_str)))
        return E_OUTOFMEMORY;
    return S_OK;
}

static HRESULT WINAPI canvas_context_measureText(IWineMSHTMLCanvasRenderingContext2D *iface, BSTR text, IDispatch **ret)
{
    struct canvas_context *context = impl_from_IWineMSHTMLCanvasRenderingContext2D(iface);
    struct text_metrics *metrics;
    SIZE size = {0};
    HFONT font, prev_font;
    HDC hdc;

    TRACE("(%p)->(%s %p)\n", context, debugstr_w(text), ret);

    if(SysStringLen(text)) {
        if(!(hdc = CreateCompatibleDC(NULL)))
            return E_OUTOFMEMORY;

        font = CreateFontW(-(int)floorf(context->font.size + 0.5f), 0, 0, 0, context->font.weight,
                           context->font.italic, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                           context->font.face);
        if(!font) {
            DeleteDC(hdc);
            return E_OUTOFMEMORY;
        }

        prev_font = SelectObject(hdc, font);
        GetTextExtentPoint32W(hdc, text, SysStringLen(text), &size);
        SelectObject(hdc, prev_font);
        DeleteObject(font);
        DeleteDC(hdc);
    }

    if(!(metrics = calloc(1, sizeof(*metrics))))
        return E_OUTOFMEMORY;

    metrics->IWineMSHTMLTextMetrics_iface.lpVtbl = &text_metrics_vtbl;
    metrics->width = size.cx;
    init_dispatch_with_owner(&metrics->dispex, &TextMetrics_dispex, &context->dispex);

    *ret = (IDispatch*)&metrics->IWineMSHTMLTextMetrics_iface;
    return S_OK;
}

static const IWineMSHTMLCanvasRenderingContext2DVtbl canvas_context_vtbl = {
    canvas_context_QueryInterface,
    canvas_context_AddRef,
    canvas_context_Release,
    canvas_context_GetTypeInfoCount,
    canvas_context_GetTypeInfo,
    canvas_context_GetIDsOfNames,
    canvas_context_Invoke,
    canvas_context_put_font,
    canvas_context_get_font,
    canvas_context_measureText
};

static inline struct canvas_context *canvas_context_from_DispatchEx(DispatchEx *iface)
{
    return CONTAINING_RECORD(iface, struct canvas_context, dispex);
}

static void *CanvasRenderingContext2D_query_interface(DispatchEx *dispex, REFIID riid)
{
    struct canvas_context *context = canvas_context_from_DispatchEx(dispex);

    if(IsEqualGUID(&IID_IWineMSHTMLCanvasRenderingContext2D, riid))
        return &context->IWineMSHTMLCanvasRenderingContext2D_iface;

    return NULL;
}

static void CanvasRenderingContext2D_destructor(DispatchEx *dispex)
{
    struct canvas_context *context = canvas_context_from_DispatchEx(dispex);
    free(context->font_str);
    free(context);
}

static const dispex_static_data_vtbl_t CanvasRenderingContext2D_dispex_vtbl = {
    .query_interface  = CanvasRenderingContext2D_query_interface,
    .destructor       = CanvasRenderingContext2D_destructor,
};

static const tid_t CanvasRenderingContext2D_iface_tids[] = {
    IWineMSHTMLCanvasRenderingContext2D_tid,
    0
};
dispex_static_data_t CanvasRenderingContext2D_dispex = {
    .id              = OBJID_CanvasRenderingContext2D,
    .vtbl            = &CanvasRenderingContext2D_dispex_vtbl,
    .disp_tid        = IWineMSHTMLCanvasRenderingContext2D_tid,
    .iface_tids      = CanvasRenderingContext2D_iface_tids,
    .min_compat_mode = COMPAT_MODE_IE9,
};

static HRESULT create_canvas_context(DispatchEx *owner, struct canvas_context **ret)
{
    struct canvas_context *context;

    if(!(context = calloc(1, sizeof(*context))))
        return E_OUTOFMEMORY;

    /* The font of a new context is "10px sans-serif". */
    if(!(context->font_str = wcsdup(L"10px sans-serif")) || !parse_canvas_font(context->font_str, &context->font)) {
        free(context->font_str);
        free(context);
        return E_OUTOFMEMORY;
    }

    context->IWineMSHTMLCanvasRenderingContext2D_iface.lpVtbl = &canvas_context_vtbl;
    init_dispatch_with_owner(&context->dispex, &CanvasRenderingContext2D_dispex, owner);

    *ret = context;
    return S_OK;
}

/* The canvas element */

static inline HTMLCanvasElement *impl_from_IWineHTMLCanvasPrivate(IWineHTMLCanvasPrivate *iface)
{
    return CONTAINING_RECORD(iface, HTMLCanvasElement, IWineHTMLCanvasPrivate_iface);
}

DISPEX_IDISPATCH_IMPL(HTMLCanvasElement_private, IWineHTMLCanvasPrivate,
                      impl_from_IWineHTMLCanvasPrivate(iface)->element.node.event_target.dispex)

static HRESULT WINAPI HTMLCanvasElement_private_getContext(IWineHTMLCanvasPrivate *iface, BSTR context_id, IDispatch **ret)
{
    HTMLCanvasElement *This = impl_from_IWineHTMLCanvasPrivate(iface);
    HRESULT hres;

    TRACE("(%p)->(%s %p)\n", This, debugstr_w(context_id), ret);

    *ret = NULL;

    if(!context_id || wcscmp(context_id, L"2d")) {
        /* An unknown context type gives null. */
        if(context_id && *context_id)
            FIXME("Unsupported context %s\n", debugstr_w(context_id));
        return S_OK;
    }

    if(!This->context) {
        hres = create_canvas_context(&This->element.node.event_target.dispex, &This->context);
        if(FAILED(hres))
            return hres;
        FIXME("Only font and measureText() of the 2D context are implemented\n");
    }

    *ret = (IDispatch*)&This->context->IWineMSHTMLCanvasRenderingContext2D_iface;
    IDispatch_AddRef(*ret);
    return S_OK;
}

static const IWineHTMLCanvasPrivateVtbl WineHTMLCanvasPrivateVtbl = {
    HTMLCanvasElement_private_QueryInterface,
    HTMLCanvasElement_private_AddRef,
    HTMLCanvasElement_private_Release,
    HTMLCanvasElement_private_GetTypeInfoCount,
    HTMLCanvasElement_private_GetTypeInfo,
    HTMLCanvasElement_private_GetIDsOfNames,
    HTMLCanvasElement_private_Invoke,
    HTMLCanvasElement_private_getContext
};

static inline HTMLCanvasElement *impl_from_DispatchEx(DispatchEx *iface)
{
    return CONTAINING_RECORD(iface, HTMLCanvasElement, element.node.event_target.dispex);
}

static void *HTMLCanvasElement_query_interface(DispatchEx *dispex, REFIID riid)
{
    HTMLCanvasElement *This = impl_from_DispatchEx(dispex);

    if(IsEqualGUID(&IID_IWineHTMLCanvasPrivate, riid))
        return &This->IWineHTMLCanvasPrivate_iface;

    return HTMLElement_query_interface(&This->element.node.event_target.dispex, riid);
}

static void HTMLCanvasElement_traverse(DispatchEx *dispex, nsCycleCollectionTraversalCallback *cb)
{
    HTMLCanvasElement *This = impl_from_DispatchEx(dispex);
    HTMLElement_traverse(dispex, cb);

    if(This->context)
        note_cc_edge((nsISupports*)&This->context->IWineMSHTMLCanvasRenderingContext2D_iface, "context", cb);
}

static void HTMLCanvasElement_unlink(DispatchEx *dispex)
{
    HTMLCanvasElement *This = impl_from_DispatchEx(dispex);
    HTMLElement_unlink(dispex);

    if(This->context) {
        struct canvas_context *context = This->context;
        This->context = NULL;
        IWineMSHTMLCanvasRenderingContext2D_Release(&context->IWineMSHTMLCanvasRenderingContext2D_iface);
    }
}

static const NodeImplVtbl HTMLCanvasElementImplVtbl = {
    .clsid                 = &CLSID_HTMLUnknownElement,
    .cpc_entries           = HTMLElement_cpc,
    .clone                 = HTMLElement_clone,
    .get_attr_col          = HTMLElement_get_attr_col
};

static void HTMLCanvasElement_init_dispex_info(dispex_data_t *info, compat_mode_t mode)
{
    /* Internet Explorer has canvas from version 9 on. */
    if(mode >= COMPAT_MODE_IE9)
        dispex_info_add_interface(info, IWineHTMLCanvasPrivate_tid, NULL);

    HTMLElement_init_dispex_info(info, mode);
}

static const event_target_vtbl_t HTMLCanvasElement_event_target_vtbl = {
    {
        HTMLELEMENT_DISPEX_VTBL_ENTRIES,
        .query_interface= HTMLCanvasElement_query_interface,
        .destructor     = HTMLElement_destructor,
        .traverse       = HTMLCanvasElement_traverse,
        .unlink         = HTMLCanvasElement_unlink
    },
    HTMLELEMENT_EVENT_TARGET_VTBL_ENTRIES,
    .handle_event       = HTMLElement_handle_event
};

dispex_static_data_t HTMLCanvasElement_dispex = {
    .id           = OBJID_HTMLCanvasElement,
    .prototype_id = OBJID_HTMLElement,
    .vtbl         = &HTMLCanvasElement_event_target_vtbl.dispex_vtbl,
    .disp_tid     = DispHTMLUnknownElement_tid,
    .init_info    = HTMLCanvasElement_init_dispex_info,
};

HRESULT HTMLCanvasElement_Create(HTMLDocumentNode *doc, nsIDOMElement *nselem, HTMLElement **elem)
{
    HTMLCanvasElement *ret;

    if(!(ret = calloc(1, sizeof(*ret))))
        return E_OUTOFMEMORY;

    ret->IWineHTMLCanvasPrivate_iface.lpVtbl = &WineHTMLCanvasPrivateVtbl;
    ret->element.node.vtbl = &HTMLCanvasElementImplVtbl;

    HTMLElement_Init(&ret->element, doc, nselem, &HTMLCanvasElement_dispex);

    *elem = &ret->element;
    return S_OK;
}
